// FAT32 on a block device, mounted into the VFS (see neko/fat.hpp and the FsOps notes in neko/vfs.hpp).
//
//   [reserved: boot sector, FSInfo, backup boot sector][FAT #1][FAT #2][data: clusters 2…]
//
// At mount the whole directory tree is read into VFS nodes; each node carries an Entry (first
// cluster, and where its 32-byte directory entry lives). Every change is written through at once:
// file data, the FAT (both copies, via a small sector cache), directory entries, sizes — so nothing
// is lost on power-off and closing a file needs no I/O. FSInfo (free-cluster hint) is written on sync.
// Long file names (VFAT) are read and written; plain 8.3 names in one case are stored without them.
// One mutex per volume serialises all of it.
#include <neko/block.hpp>
#include <neko/fat.hpp>
#include <neko/process.hpp>
#include <neko/runtime.hpp>
#include <neko/vfs.hpp>

namespace neko::fat {
namespace {

using namespace proc;  // error codes

constexpr uint32_t kMask = 0x0FFFFFFF;
constexpr uint32_t kEnd = 0x0FFFFFF8;  // ≥ this: end of chain
constexpr uint32_t kEndMark = 0x0FFFFFFF;
constexpr uint8_t kAttrVolume = 0x08, kAttrDirectory = 0x10, kAttrArchive = 0x20, kAttrLfn = 0x0F;
constexpr uint8_t kNtLowerBase = 0x08, kNtLowerExt = 0x10;
constexpr uint32_t kMinClusters = 65525;  // fewer and every other implementation reads it as FAT16
constexpr uint16_t kDate = ((2026 - 1980) << 9) | (1 << 5) | 1;  // no RTC yet: 2026-01-01
constexpr uint32_t kCacheSlots = 16;

struct CacheSlot {
    uint64_t sector;  // FAT sector number (within one FAT copy)
    uint64_t stamp;
    bool valid;
    uint8_t* data;
};

struct Volume {
    uint32_t block;
    uint32_t bps;            // bytes per sector
    uint32_t cluster_bytes;
    uint64_t fat_offset;     // bytes
    uint64_t fat_bytes;      // one copy
    uint32_t fats;
    uint64_t data_offset;    // bytes, cluster 2
    uint32_t clusters;       // data clusters: numbers 2 … clusters + 1
    uint32_t root_cluster;
    uint32_t fsinfo_sector;
    uint32_t free_count;
    uint32_t next_free;
    sync::Mutex lock;
    CacheSlot cache[kCacheSlots];
    uint64_t stamp;
    uint8_t* zero;           // one zeroed cluster
    char name[16];
};

struct Entry {
    uint32_t first;      // first cluster, 0 = no data yet
    uint32_t dir_first;  // first cluster of the directory holding the entry
    uint32_t slot;       // index of the 32-byte short entry in that directory
    uint32_t lfn;        // long-name slots right before it
    uint8_t attr;
    bool root;
    uint32_t pos_index;  // last cluster looked up: index in the chain → cluster (sequential I/O)
    uint32_t pos_cluster;
};

struct Guard {
    Volume* v;
    explicit Guard(Volume* volume) : v(volume) { v->lock.lock(); }
    ~Guard() { v->lock.unlock(); }
};

uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
void wr16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}
void wr32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}

// ---------------------------------------------------------------- disk

int64_t disk_read(Volume* v, uint64_t offset, void* buffer, uint64_t length) {
    int64_t n = block::read_bytes(v->block, offset, buffer, length);
    return n == int64_t(length) ? 0 : n < 0 ? n : kErrIo;
}

int64_t disk_write(Volume* v, uint64_t offset, const void* buffer, uint64_t length) {
    int64_t n = block::write_bytes(v->block, offset, buffer, length);
    return n == int64_t(length) ? 0 : n < 0 ? n : kErrIo;
}

uint64_t cluster_offset(Volume* v, uint32_t c) { return v->data_offset + uint64_t(c - 2) * v->cluster_bytes; }
bool valid_cluster(Volume* v, uint32_t c) { return c >= 2 && c < v->clusters + 2; }

// ---------------------------------------------------------------- FAT

uint8_t* fat_sector(Volume* v, uint64_t sector) {
    CacheSlot* victim = &v->cache[0];
    for (CacheSlot& s : v->cache) {
        if (s.valid && s.sector == sector) {
            s.stamp = ++v->stamp;
            return s.data;
        }
        if (!s.valid || s.stamp < victim->stamp) victim = &s;
    }
    if (!victim->data && !(victim->data = static_cast<uint8_t*>(heap::alloc(v->bps)))) return nullptr;
    victim->valid = false;
    if (disk_read(v, v->fat_offset + sector * v->bps, victim->data, v->bps) < 0) return nullptr;
    victim->valid = true;
    victim->sector = sector;
    victim->stamp = ++v->stamp;
    return victim->data;
}

// Next cluster after `c` (or kEndMark on a read error, which ends any walk).
uint32_t fat_get(Volume* v, uint32_t c) {
    uint64_t at = uint64_t(c) * 4;
    uint8_t* s = fat_sector(v, at / v->bps);
    return s ? rd32(s + at % v->bps) & kMask : kEndMark;
}

int64_t fat_set(Volume* v, uint32_t c, uint32_t value) {
    uint64_t at = uint64_t(c) * 4;
    uint64_t sector = at / v->bps;
    uint8_t* s = fat_sector(v, sector);
    if (!s) return kErrIo;
    uint8_t* p = s + at % v->bps;
    wr32(p, (rd32(p) & ~kMask) | (value & kMask));
    for (uint32_t i = 0; i < v->fats; ++i)  // write-through, every copy
        if (int64_t e = disk_write(v, v->fat_offset + i * v->fat_bytes + sector * v->bps, s, v->bps)) return e;
    return 0;
}

// A free cluster marked end-of-chain (zeroed on disk when `zero`); 0 when the disk is full.
uint32_t alloc_cluster(Volume* v, bool zero) {
    if (v->free_count == 0) return 0;
    uint32_t start = valid_cluster(v, v->next_free) ? v->next_free : 2;
    for (uint32_t i = 0; i < v->clusters; ++i) {
        uint32_t c = 2 + (start - 2 + i) % v->clusters;
        if (fat_get(v, c) != 0) continue;
        if (zero && disk_write(v, cluster_offset(v, c), v->zero, v->cluster_bytes) < 0) return 0;
        if (fat_set(v, c, kEndMark) < 0) return 0;
        --v->free_count;
        v->next_free = c + 1;
        return c;
    }
    v->free_count = 0;
    return 0;
}

void free_chain(Volume* v, uint32_t c) {
    for (uint32_t guard = 0; valid_cluster(v, c) && guard < v->clusters; ++guard) {
        uint32_t next = fat_get(v, c);
        if (fat_set(v, c, 0) < 0) return;
        ++v->free_count;
        if (c < v->next_free) v->next_free = c;
        c = next;
    }
}

// Cluster number `index` of a chain, appending zeroed clusters when `extend`. 0 past the end.
uint32_t chain_at(Volume* v, uint32_t first, uint32_t index, bool extend, uint32_t* hint_index = nullptr,
                  uint32_t* hint_cluster = nullptr) {
    uint32_t i = 0, c = first;
    if (hint_index && *hint_cluster && *hint_index <= index) {
        i = *hint_index;
        c = *hint_cluster;
    }
    while (i < index) {
        uint32_t next = fat_get(v, c);
        if (!valid_cluster(v, next)) {
            if (next < kEnd || !extend) return 0;  // free/bad in the middle of a chain, or its end
            next = alloc_cluster(v, true);
            if (!next || fat_set(v, c, next) < 0) return 0;
        }
        c = next;
        ++i;
    }
    if (hint_index) {
        *hint_index = i;
        *hint_cluster = c;
    }
    return c;
}

// ---------------------------------------------------------------- directory slots

// Disk offset of 32-byte slot `slot` of the directory starting at `first` (grows it when `extend`).
int64_t slot_offset(Volume* v, uint32_t first, uint32_t slot, bool extend, uint64_t& out) {
    uint64_t byte = uint64_t(slot) * 32;
    uint32_t c = chain_at(v, first, uint32_t(byte / v->cluster_bytes), extend);
    if (!c) return extend ? kErrNoMemory : kErrNotFound;
    out = cluster_offset(v, c) + byte % v->cluster_bytes;
    return 0;
}

int64_t read_slot(Volume* v, uint32_t first, uint32_t slot, uint8_t* out) {
    uint64_t at;
    if (int64_t e = slot_offset(v, first, slot, false, at)) return e;
    return disk_read(v, at, out, 32);
}

int64_t write_slot(Volume* v, uint32_t first, uint32_t slot, const uint8_t* data) {
    uint64_t at;
    if (int64_t e = slot_offset(v, first, slot, true, at)) return e;
    return disk_write(v, at, data, 32);
}

// Rewrites the first cluster and size in a node's short entry.
int64_t write_entry(Volume* v, Entry* e, uint64_t size) {
    if (e->root) return 0;
    uint8_t s[32];
    if (int64_t r = read_slot(v, e->dir_first, e->slot, s)) return r;
    wr16(s + 20, uint16_t(e->first >> 16));
    wr16(s + 26, uint16_t(e->first));
    wr32(s + 28, (e->attr & kAttrDirectory) ? 0 : uint32_t(size));
    wr16(s + 22, 0);
    wr16(s + 24, kDate);
    return write_slot(v, e->dir_first, e->slot, s);
}

// Calls fn(slot index, 32 bytes, ctx) for every slot until fn returns false or the end marker.
// Returns the number of slots the directory has room for (whole clusters) when it ran to the end.
template <class F>
int64_t each_slot(Volume* v, uint32_t first, F fn) {
    auto* buffer = static_cast<uint8_t*>(heap::alloc(v->cluster_bytes));
    if (!buffer) return kErrNoMemory;
    uint32_t per_cluster = v->cluster_bytes / 32;
    uint32_t c = first, index = 0;
    int64_t result = 0;
    for (uint32_t guard = 0; valid_cluster(v, c) && guard < v->clusters; ++guard) {
        if ((result = disk_read(v, cluster_offset(v, c), buffer, v->cluster_bytes)) < 0) break;
        bool stop = false;
        for (uint32_t i = 0; i < per_cluster && !stop; ++i, ++index) stop = !fn(index, buffer + i * 32);
        if (stop) {
            result = -100;  // stopped early (not an error for the callers)
            break;
        }
        c = fat_get(v, c);
        result = index;
    }
    heap::free(buffer);
    return result == -100 ? 0 : result;
}

// ---------------------------------------------------------------- names

uint8_t lfn_checksum(const uint8_t* short_name) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; ++i) sum = uint8_t(((sum & 1) << 7) + (sum >> 1) + short_name[i]);
    return sum;
}

bool short_char(char c) {
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
    for (const char* p = "!#$%&'()-@^_`{}~"; *p; ++p)
        if (*p == c) return true;
    return false;
}

char upper(char c) { return c >= 'a' && c <= 'z' ? char(c - 32) : c; }

// Fits `name` into an 8.3 entry exactly (letters of one case per part)? Fills the 11 bytes and NT flags.
bool exact_short(const char* name, uint8_t* out, uint8_t& nt) {
    mem::set(out, ' ', 11);
    nt = 0;
    const char* dot = nullptr;
    for (const char* p = name; *p; ++p)
        if (*p == '.') {
            if (dot) return false;
            dot = p;
        }
    uint32_t base_len = uint32_t(dot ? dot - name : str::length(name));
    uint32_t ext_len = dot ? uint32_t(str::length(dot + 1)) : 0;
    if (base_len == 0 || base_len > 8 || ext_len > 3 || (dot && ext_len == 0)) return false;
    auto part = [&](const char* s, uint32_t n, uint8_t* to, uint8_t lower_flag) {
        bool lower = false, upper_seen = false;
        for (uint32_t i = 0; i < n; ++i) {
            char c = s[i];
            if (c >= 'a' && c <= 'z') lower = true;
            else if (c >= 'A' && c <= 'Z') upper_seen = true;
            if (!short_char(upper(c))) return false;
            to[i] = uint8_t(upper(c));
        }
        if (lower && upper_seen) return false;
        if (lower) nt |= lower_flag;
        return true;
    };
    if (!part(name, base_len, out, kNtLowerBase)) return false;
    if (dot && !part(dot + 1, ext_len, out + 8, kNtLowerExt)) return false;
    if (out[0] == 0xE5) out[0] = 0x05;
    return true;
}

// "LONGNA~N.EXT" for a name that needs a long entry; N makes it unique in the directory.
int64_t alias(Volume* v, uint32_t dir_first, const char* name, uint8_t* out) {
    const char* dot = nullptr;
    for (const char* p = name; *p; ++p)
        if (*p == '.') dot = p;
    if (dot == name) dot = nullptr;  // ".profile": no extension
    uint8_t base[8], ext[3];
    uint32_t nb = 0, ne = 0;
    for (const char* p = name; *p && (!dot || p < dot) && nb < 6; ++p)
        if (short_char(upper(*p))) base[nb++] = uint8_t(upper(*p));
    if (nb == 0) base[nb++] = '_';
    if (dot)
        for (const char* p = dot + 1; *p && ne < 3; ++p)
            if (short_char(upper(*p))) ext[ne++] = uint8_t(upper(*p));
    for (uint32_t n = 1; n < 1000000; ++n) {
        mem::set(out, ' ', 11);
        char digits[8];
        uint32_t nd = 0;
        for (uint32_t x = n; x; x /= 10) digits[nd++] = char('0' + x % 10);
        uint32_t keep = nb < 7 - nd ? nb : 7 - nd;
        mem::copy(out, base, keep);
        out[keep] = '~';
        for (uint32_t i = 0; i < nd; ++i) out[keep + 1 + i] = uint8_t(digits[nd - 1 - i]);
        mem::copy(out + 8, ext, ne);
        bool taken = false;
        int64_t e = each_slot(v, dir_first, [&](uint32_t, const uint8_t* s) {
            if (s[0] == 0) return false;
            if (s[0] != 0xE5 && s[11] != kAttrLfn && mem::compare(s, out, 11) == 0) taken = true;
            return !taken;
        });
        if (e < 0) return e;
        if (!taken) return 0;
    }
    return kErrExists;
}

// UTF-8 → UTF-16 (BMP); length in code units or -1.
int32_t to_utf16(const char* s, uint16_t* out, int32_t capacity) {
    int32_t n = 0;
    auto* p = reinterpret_cast<const uint8_t*>(s);
    while (*p) {
        uint32_t cp;
        if (*p < 0x80) cp = *p++;
        else if ((*p & 0xE0) == 0xC0 && p[1]) cp = ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3F), p += 2;
        else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) cp = ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3F), p += 3;
        else return -1;  // 4-byte UTF-8 would need surrogates: not supported
        if (n == capacity) return -1;
        out[n++] = uint16_t(cp);
    }
    return n;
}

// UTF-16 → UTF-8 into `out` (kMaxName + 1 bytes); false if it does not fit.
bool from_utf16(const uint16_t* s, int32_t n, char* out) {
    uint32_t at = 0;
    for (int32_t i = 0; i < n; ++i) {
        uint32_t c = s[i];
        uint32_t need = c < 0x80 ? 1 : c < 0x800 ? 2 : 3;
        if (at + need > vfs::kMaxName) return false;
        if (need == 1) out[at++] = char(c);
        else if (need == 2) {
            out[at++] = char(0xC0 | (c >> 6));
            out[at++] = char(0x80 | (c & 0x3F));
        } else {
            out[at++] = char(0xE0 | (c >> 12));
            out[at++] = char(0x80 | ((c >> 6) & 0x3F));
            out[at++] = char(0x80 | (c & 0x3F));
        }
    }
    out[at] = 0;
    return at > 0;
}

void short_to_name(const uint8_t* s, char* out) {
    uint32_t n = 0;
    uint8_t nt = s[12];
    for (int i = 0; i < 8 && s[i] != ' '; ++i) {
        char c = char(i == 0 && s[0] == 0x05 ? 0xE5 : s[i]);
        out[n++] = (nt & kNtLowerBase) && c >= 'A' && c <= 'Z' ? char(c + 32) : c;
    }
    if (s[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && s[i] != ' '; ++i) {
            char c = char(s[i]);
            out[n++] = (nt & kNtLowerExt) && c >= 'A' && c <= 'Z' ? char(c + 32) : c;
        }
    }
    out[n] = 0;
}

const uint8_t kLfnOffsets[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};

// Collects the long name in front of a short entry while walking a directory.
struct LfnState {
    uint16_t chars[260];
    uint8_t checksum;
    uint8_t expected;  // next sequence number we want (counts down to 1)
    uint8_t count;
    bool valid;

    void reset() { valid = false; expected = 0; }
    void add(const uint8_t* s) {
        uint8_t seq = s[0] & 0x1F;
        if (s[0] & 0x40) {
            if (seq == 0 || seq > 20) return reset();
            count = seq;
            checksum = s[13];
            valid = true;
            for (uint16_t& c : chars) c = 0xFFFF;
        } else if (!valid || seq != expected || s[13] != checksum) {
            return reset();
        }
        for (int k = 0; k < 13; ++k) chars[(seq - 1) * 13 + k] = rd16(s + kLfnOffsets[k]);
        expected = uint8_t(seq - 1);
    }
    // The name for short entry `s`, if a complete long name precedes it.
    bool name(const uint8_t* s, char* out) {
        if (!valid || expected != 0 || lfn_checksum(s) != checksum) return false;
        int32_t n = 0;
        while (n < count * 13 && chars[n] != 0 && chars[n] != 0xFFFF) ++n;
        return from_utf16(chars, n, out);
    }
};

// ---------------------------------------------------------------- mount: the tree

int64_t scan(Volume* v, vfs::Node* dir, uint32_t first, uint32_t depth) {
    if (depth > 32) return 0;
    struct Sub {
        vfs::Node* node;
        uint32_t first;
    };
    Sub subs[64];
    uint32_t nsubs = 0;
    LfnState lfn;
    lfn.reset();
    uint32_t lfn_slots = 0;
    int64_t error = 0;
    int64_t r = each_slot(v, first, [&](uint32_t index, const uint8_t* s) {
        if (s[0] == 0) return false;
        if (s[0] == 0xE5) {
            lfn.reset();
            lfn_slots = 0;
            return true;
        }
        if (s[11] == kAttrLfn) {
            lfn.add(s);
            ++lfn_slots;
            return true;
        }
        char name[vfs::kMaxName + 1];
        bool skip = (s[11] & kAttrVolume) || (s[0] == '.' && (s[1] == ' ' || s[1] == '.'));
        bool has_long = lfn.name(s, name);
        uint32_t lfns = has_long ? lfn_slots : 0;
        lfn.reset();
        lfn_slots = 0;
        if (skip) return true;
        if (!has_long) short_to_name(s, name);
        auto* e = static_cast<Entry*>(heap::alloc_zeroed(sizeof(Entry)));
        if (!e) {
            error = kErrNoMemory;
            return false;
        }
        e->first = (uint32_t(rd16(s + 20)) << 16) | rd16(s + 26);
        e->dir_first = first;
        e->slot = index;
        e->lfn = lfns;
        e->attr = s[11];
        bool is_dir = s[11] & kAttrDirectory;
        if (vfs::has_child(dir, name)) {  // a broken volume with duplicates: keep the first
            heap::free(e);
            return true;
        }
        vfs::Node* n = vfs::add_node(dir, name, is_dir ? vfs::Type::Directory : vfs::Type::File, is_dir ? 0 : rd32(s + 28), e, false);
        if (!n) {
            heap::free(e);
            error = kErrNoMemory;
            return false;
        }
        if (is_dir && valid_cluster(v, e->first)) {
            if (nsubs == 64) {  // flush: descend now (keeps the stack bounded)
                for (uint32_t i = 0; i < nsubs && error == 0; ++i) error = scan(v, subs[i].node, subs[i].first, depth + 1);
                nsubs = 0;
            }
            subs[nsubs++] = {n, e->first};
        }
        return error == 0;
    });
    if (r < 0) return r;
    for (uint32_t i = 0; i < nsubs && error == 0; ++i) error = scan(v, subs[i].node, subs[i].first, depth + 1);
    return error;
}

// ---------------------------------------------------------------- FsOps

Entry* entry_of(vfs::Node* n) { return static_cast<Entry*>(vfs::node_data(n)); }

// Writes `length` bytes at `offset` (buffer null: zeros), growing the chain; no size update.
int64_t write_range(Volume* v, vfs::Node* n, Entry* e, uint64_t offset, const uint8_t* buffer, uint64_t length) {
    if (!e->first) {
        uint32_t c = alloc_cluster(v, true);
        if (!c) return kErrNoMemory;
        e->first = c;
        e->pos_index = 0;
        e->pos_cluster = c;
        if (int64_t r = write_entry(v, e, vfs::node_size(n))) return r;
    }
    uint64_t done = 0;
    while (done < length) {
        uint64_t at = offset + done;
        uint32_t c = chain_at(v, e->first, uint32_t(at / v->cluster_bytes), true, &e->pos_index, &e->pos_cluster);
        if (!c) return done ? int64_t(done) : kErrNoMemory;
        uint64_t within = at % v->cluster_bytes;
        uint64_t chunk = v->cluster_bytes - within;
        if (chunk > length - done) chunk = length - done;
        int64_t r = disk_write(v, cluster_offset(v, c) + within, buffer ? buffer + done : v->zero, chunk);
        if (r < 0) return done ? int64_t(done) : r;
        done += chunk;
    }
    return int64_t(done);
}

int64_t op_read(void* fs, vfs::Node* n, uint64_t offset, void* buffer, uint64_t length) {
    auto* v = static_cast<Volume*>(fs);
    Guard guard(v);
    Entry* e = entry_of(n);
    uint64_t size = vfs::node_size(n);
    if (offset >= size) return 0;
    if (length > size - offset) length = size - offset;
    auto* out = static_cast<uint8_t*>(buffer);
    uint64_t done = 0;
    while (done < length) {
        uint64_t at = offset + done;
        uint32_t c = chain_at(v, e->first, uint32_t(at / v->cluster_bytes), false, &e->pos_index, &e->pos_cluster);
        if (!c) return done ? int64_t(done) : kErrIo;  // chain shorter than the size says
        uint64_t within = at % v->cluster_bytes;
        uint64_t chunk = v->cluster_bytes - within;
        if (chunk > length - done) chunk = length - done;
        // Following clusters that sit right behind on disk are read in the same request.
        uint32_t last = c;
        while (chunk < length - done && chunk < 256 * 1024) {
            uint32_t next = fat_get(v, last);
            if (next != last + 1) break;
            uint64_t more = v->cluster_bytes;
            if (more > length - done - chunk) more = length - done - chunk;
            chunk += more;
            last = next;
            ++e->pos_index;
            e->pos_cluster = last;
        }
        int64_t r = disk_read(v, cluster_offset(v, c) + within, out + done, chunk);
        if (r < 0) return done ? int64_t(done) : r;
        done += chunk;
    }
    return int64_t(done);
}

int64_t op_write(void* fs, vfs::Node* n, uint64_t offset, const void* buffer, uint64_t length) {
    auto* v = static_cast<Volume*>(fs);
    Guard guard(v);
    Entry* e = entry_of(n);
    uint64_t size = vfs::node_size(n);
    if (offset + length > 0xFFFFFFFFull) return kErrNoMemory;  // FAT32 files end at 4 GiB
    if (offset > size) {  // the gap after seeking past the end reads as zeros
        int64_t r = write_range(v, n, e, size, nullptr, offset - size);
        if (r < 0) return r;
    }
    int64_t r = write_range(v, n, e, offset, static_cast<const uint8_t*>(buffer), length);
    if (r <= 0) return r;
    uint64_t end = offset + uint64_t(r);
    if (end > size) {
        vfs::set_size(n, end);
        if (int64_t w = write_entry(v, e, end)) return w;
    }
    return r;
}

int64_t op_truncate(void* fs, vfs::Node* n, uint64_t size) {
    auto* v = static_cast<Volume*>(fs);
    Guard guard(v);
    Entry* e = entry_of(n);
    uint64_t current = vfs::node_size(n);
    if (size > 0xFFFFFFFFull) return kErrNoMemory;
    if (size > current) {
        int64_t r = write_range(v, n, e, current, nullptr, size - current);
        if (r < 0) return r;
    } else if (size < current) {
        uint32_t keep = uint32_t((size + v->cluster_bytes - 1) / v->cluster_bytes);
        if (keep == 0) {
            free_chain(v, e->first);
            e->first = 0;
        } else if (uint32_t last = chain_at(v, e->first, keep - 1, false)) {
            uint32_t rest = fat_get(v, last);
            fat_set(v, last, kEndMark);
            if (valid_cluster(v, rest)) free_chain(v, rest);
        }
        e->pos_index = 0;
        e->pos_cluster = 0;
    }
    vfs::set_size(n, size);
    return write_entry(v, e, size);
}

// Writes the long-name slots and the short entry `s` for `name` at a free run in directory `dir_first`.
// Fills slot / lfn of `e`.
int64_t place_entry(Volume* v, uint32_t dir_first, const char* name, uint8_t* s, Entry* e) {
    uint8_t nt = 0;
    uint16_t units[256];
    int32_t nunits = 0;
    bool exact = exact_short(name, s, nt);
    if (!exact) {
        nunits = to_utf16(name, units, 255);
        if (nunits <= 0) return kErrBadArgument;
        if (int64_t r = alias(v, dir_first, name, s)) return r;
        nt = 0;
    }
    s[12] = nt;
    uint32_t lfns = exact ? 0 : uint32_t((nunits + 12) / 13);
    uint32_t need = lfns + 1;

    // First run of `need` free slots (deleted, or the end marker and everything after it).
    uint32_t run_start = 0, run = 0;
    bool found = false;
    int64_t total = each_slot(v, dir_first, [&](uint32_t index, const uint8_t* slot) {
        if (slot[0] == 0) {  // end: everything from here on is free
            if (run == 0) run_start = index;
            found = true;
            return false;
        }
        if (slot[0] == 0xE5) {
            if (run++ == 0) run_start = index;
            if (run == need) {
                found = true;
                return false;
            }
        } else {
            run = 0;
        }
        return true;
    });
    if (total < 0) return total;
    if (!found) run_start = run ? run_start : uint32_t(total);  // continue into a new cluster
    if (run_start + need > 65536) return kErrLimit;           // FAT directories hold 65536 entries

    uint8_t sum = lfn_checksum(s);
    for (uint32_t i = 0; i < lfns; ++i) {
        uint32_t seq = lfns - i;  // physical order: last part first
        uint8_t l[32];
        mem::set(l, 0, sizeof(l));
        l[0] = uint8_t(seq | (i == 0 ? 0x40 : 0));
        l[11] = kAttrLfn;
        l[13] = sum;
        for (int k = 0; k < 13; ++k) {
            int32_t at = int32_t(seq - 1) * 13 + k;
            uint16_t c = at < nunits ? units[at] : at == nunits ? 0 : 0xFFFF;
            wr16(l + kLfnOffsets[k], c);
        }
        if (int64_t r = write_slot(v, dir_first, run_start + i, l)) return r;
    }
    if (int64_t r = write_slot(v, dir_first, run_start + lfns, s)) return r;
    e->dir_first = dir_first;
    e->slot = run_start + lfns;
    e->lfn = lfns;
    return 0;
}

int64_t erase_entry(Volume* v, Entry* e) {
    for (uint32_t i = 0; i <= e->lfn; ++i) {
        uint8_t s[32];
        uint32_t slot = e->slot - e->lfn + i;
        if (int64_t r = read_slot(v, e->dir_first, slot, s)) return r;
        s[0] = 0xE5;
        if (int64_t r = write_slot(v, e->dir_first, slot, s)) return r;
    }
    return 0;
}

void make_dot(uint8_t* s, const char* name, uint32_t cluster) {
    mem::set(s, 0, 32);
    mem::set(s, ' ', 11);
    mem::copy(s, name, str::length(name));
    s[11] = kAttrDirectory;
    wr16(s + 16, kDate);
    wr16(s + 24, kDate);
    wr16(s + 20, uint16_t(cluster >> 16));
    wr16(s + 26, uint16_t(cluster));
}

int64_t op_create(void* fs, vfs::Node* dir, const char* name, vfs::Type type, vfs::Node*& out) {
    auto* v = static_cast<Volume*>(fs);
    Guard guard(v);
    if (type != vfs::Type::File && type != vfs::Type::Directory) return kErrNotSupported;
    if (vfs::has_child(dir, name)) return kErrExists;
    Entry* d = entry_of(dir);
    auto* e = static_cast<Entry*>(heap::alloc_zeroed(sizeof(Entry)));
    if (!e) return kErrNoMemory;
    bool is_dir = type == vfs::Type::Directory;
    e->attr = is_dir ? kAttrDirectory : kAttrArchive;
    if (is_dir) {  // its own cluster with "." and ".."
        e->first = alloc_cluster(v, true);
        if (!e->first) {
            heap::free(e);
            return kErrNoMemory;
        }
        uint8_t dot[32];
        make_dot(dot, ".", e->first);
        int64_t r = write_slot(v, e->first, 0, dot);
        make_dot(dot, "..", d->root ? 0 : d->first);
        if (r == 0) r = write_slot(v, e->first, 1, dot);
        if (r < 0) {
            free_chain(v, e->first);
            heap::free(e);
            return r;
        }
    }
    uint8_t s[32];
    mem::set(s, 0, sizeof(s));
    s[11] = e->attr;
    wr16(s + 16, kDate);
    wr16(s + 18, kDate);
    wr16(s + 24, kDate);
    wr16(s + 20, uint16_t(e->first >> 16));
    wr16(s + 26, uint16_t(e->first));
    int64_t r = place_entry(v, d->first, name, s, e);
    if (r == 0) {
        out = vfs::add_node(dir, name, type, 0, e, true);
        if (!out) {
            erase_entry(v, e);
            r = kErrNoMemory;
        }
    }
    if (r < 0) {
        if (e->first) free_chain(v, e->first);
        heap::free(e);
    }
    return r;
}

int64_t op_remove(void* fs, vfs::Node* n) {
    auto* v = static_cast<Volume*>(fs);
    Guard guard(v);
    Entry* e = entry_of(n);
    if ((e->attr & kAttrDirectory) && vfs::has_children(n)) return kErrNotEmpty;
    if (int64_t r = erase_entry(v, e)) return r;
    free_chain(v, e->first);
    e->first = 0;
    vfs::detach_node(n);
    return 0;
}

int64_t op_rename(void* fs, vfs::Node* n, vfs::Node* dir, const char* name) {
    auto* v = static_cast<Volume*>(fs);
    Guard guard(v);
    if (vfs::has_child(dir, name)) return kErrExists;
    Entry* e = entry_of(n);
    Entry* d = entry_of(dir);
    uint8_t s[32];
    if (int64_t r = read_slot(v, e->dir_first, e->slot, s)) return r;
    Entry moved = *e;
    if (int64_t r = place_entry(v, d->first, name, s, &moved)) return r;
    Entry old = *e;
    *e = moved;
    erase_entry(v, &old);
    if ((e->attr & kAttrDirectory) && old.dir_first != d->first && valid_cluster(v, e->first)) {
        uint8_t dotdot[32];  // ".." follows the directory to its new parent
        if (read_slot(v, e->first, 1, dotdot) == 0) {
            uint32_t parent = d->root ? 0 : d->first;
            wr16(dotdot + 20, uint16_t(parent >> 16));
            wr16(dotdot + 26, uint16_t(parent));
            write_slot(v, e->first, 1, dotdot);
        }
    }
    vfs::move_node(n, dir, name);
    return 0;
}

int64_t write_fsinfo(Volume* v) {
    uint8_t* s = static_cast<uint8_t*>(heap::alloc(v->bps));
    if (!s) return kErrNoMemory;
    int64_t r = disk_read(v, uint64_t(v->fsinfo_sector) * v->bps, s, v->bps);
    if (r == 0 && rd32(s) == 0x41615252 && rd32(s + 484) == 0x61417272) {
        wr32(s + 488, v->free_count);
        wr32(s + 492, v->next_free);
        r = disk_write(v, uint64_t(v->fsinfo_sector) * v->bps, s, v->bps);
    }
    heap::free(s);
    return r;
}

int64_t op_sync(void* fs) {
    auto* v = static_cast<Volume*>(fs);
    Guard guard(v);
    int64_t r = write_fsinfo(v);
    int64_t f = block::flush(v->block);
    return r < 0 ? r : f;
}

void op_release(void*, void* data) { heap::free(data); }

void free_volume(Volume* v) {
    for (CacheSlot& s : v->cache) heap::free(s.data);
    heap::free(v->zero);
    heap::free(v);
}

void op_unmount(void* fs) { free_volume(static_cast<Volume*>(fs)); }

void op_usage(void* fs, uint64_t& total, uint64_t& free) {
    auto* v = static_cast<Volume*>(fs);
    total = uint64_t(v->clusters) * v->cluster_bytes;
    free = uint64_t(v->free_count) * v->cluster_bytes;
}

const vfs::FsOps kOps = {"fat32", op_read, op_write, op_truncate, op_create, op_remove, op_rename,
                         op_sync, op_release, op_unmount, op_usage};

// ---------------------------------------------------------------- boot sector

struct Layout {
    uint32_t bps, spc, reserved, fats, fat_sectors, root_cluster, fsinfo;
    uint64_t total;
};

bool parse_boot(const uint8_t* b, Layout& l) {
    if (b[510] != 0x55 || b[511] != 0xAA) return false;
    l.bps = rd16(b + 11);
    l.spc = b[13];
    l.reserved = rd16(b + 14);
    l.fats = b[16];
    uint16_t root_entries = rd16(b + 17);
    uint16_t total16 = rd16(b + 19);
    uint16_t fat16 = rd16(b + 22);
    l.total = total16 ? total16 : rd32(b + 32);
    l.fat_sectors = rd32(b + 36);
    l.root_cluster = rd32(b + 44);
    l.fsinfo = rd16(b + 48);
    bool pow2_bps = l.bps >= 512 && l.bps <= 4096 && !(l.bps & (l.bps - 1));
    bool pow2_spc = l.spc && !(l.spc & (l.spc - 1));
    return pow2_bps && pow2_spc && root_entries == 0 && fat16 == 0 && l.fat_sectors && l.fats >= 1 && l.fats <= 2 &&
           l.reserved && l.total > l.reserved + uint64_t(l.fats) * l.fat_sectors;
}

int32_t device_index(const char* device) {
    if (!device) return kErrNotFound;
    if (str::length(device) > 5 && mem::compare(device, "/dev/", 5) == 0) device += 5;
    int32_t i = block::find(device);
    return i < 0 ? kErrNotFound : i;
}

}  // namespace

bool probe(const char* device) {
    int32_t index = device_index(device);
    if (index < 0) return false;
    uint8_t boot[512];
    Layout l;
    return block::read_bytes(uint32_t(index), 0, boot, 512) == 512 && parse_boot(boot, l);
}

int64_t mount(const char* device, const char* dir) {
    int32_t index = device_index(device);
    if (index < 0) return index;
    uint8_t boot[512];
    if (block::read_bytes(uint32_t(index), 0, boot, 512) != 512) return kErrIo;
    Layout l;
    if (!parse_boot(boot, l)) return kErrNotSupported;

    auto* v = static_cast<Volume*>(heap::alloc_zeroed(sizeof(Volume)));
    if (!v) return kErrNoMemory;
    v->block = uint32_t(index);
    v->bps = l.bps;
    v->cluster_bytes = l.bps * l.spc;
    v->fats = l.fats;
    v->fat_offset = uint64_t(l.reserved) * l.bps;
    v->fat_bytes = uint64_t(l.fat_sectors) * l.bps;
    v->data_offset = v->fat_offset + v->fats * v->fat_bytes;
    uint64_t data_sectors = l.total - l.reserved - uint64_t(l.fats) * l.fat_sectors;
    uint64_t clusters = data_sectors / l.spc;
    uint64_t fat_entries = v->fat_bytes / 4;
    if (clusters + 2 > fat_entries) clusters = fat_entries - 2;
    if (clusters > 0x0FFFFFF5) clusters = 0x0FFFFFF5;
    v->clusters = uint32_t(clusters);
    v->root_cluster = l.root_cluster;
    v->fsinfo_sector = l.fsinfo;
    v->lock.set_name("fat32 volume");
    block::Info bi;
    block::info(uint32_t(index), bi);
    str::copy(v->name, reinterpret_cast<const char*>(bi.name), sizeof(v->name));
    v->zero = static_cast<uint8_t*>(heap::alloc_zeroed(v->cluster_bytes));
    auto* root = static_cast<Entry*>(heap::alloc_zeroed(sizeof(Entry)));
    if (!v->zero || !root || !valid_cluster(v, v->root_cluster)) {
        int64_t why = v->zero && root ? kErrNotSupported : kErrNoMemory;
        heap::free(root);
        free_volume(v);
        return why;
    }
    root->first = v->root_cluster;
    root->root = true;
    root->attr = kAttrDirectory;

    // Free clusters: the FSInfo hint when it is sane, otherwise count them.
    v->free_count = 0xFFFFFFFF;
    uint8_t* info = static_cast<uint8_t*>(heap::alloc(l.bps));
    if (info && l.fsinfo && disk_read(v, uint64_t(l.fsinfo) * l.bps, info, l.bps) == 0 && rd32(info) == 0x41615252 &&
        rd32(info + 484) == 0x61417272) {
        v->free_count = rd32(info + 488);
        v->next_free = rd32(info + 492);
    }
    heap::free(info);
    if (v->free_count > v->clusters) {
        v->free_count = 0;
        for (uint32_t c = 2; c < v->clusters + 2; ++c)
            if (fat_get(v, c) == 0) ++v->free_count;
    }
    if (!valid_cluster(v, v->next_free)) v->next_free = 2;

    vfs::Node* node;
    int64_t r = vfs::mount(dir, &kOps, v, v->name, root, node);
    if (r < 0) {
        heap::free(root);
        free_volume(v);
        return r;
    }
    {
        Guard guard(v);
        r = scan(v, node, v->root_cluster, 0);
    }
    if (r < 0) vfs::unmount(dir);
    return r;
}

int64_t format(const char* device, const char* label) {
    int32_t index = device_index(device);
    if (index < 0) return index;
    block::Info bi;
    block::info(uint32_t(index), bi);
    if (bi.read_only) return kErrAccess;
    uint32_t bps = bi.sector_size;
    uint64_t total = bi.sectors;
    if (total > 0xFFFFFFFFull) total = 0xFFFFFFFFull;
    // Sectors per cluster from Microsoft's table (in 512-byte units), scaled for bigger sectors.
    uint64_t bytes = total * bps;
    uint32_t cluster = bytes <= (260ull << 20) ? 512 : bytes <= (8ull << 30) ? 4096 : bytes <= (16ull << 30) ? 8192 : 16384;
    uint32_t spc = cluster / bps ? cluster / bps : 1;
    constexpr uint32_t kReserved = 32, kFats = 2;
    // FAT size: enough 4-byte entries for every cluster that remains after the FATs themselves.
    // Growing the FATs only shrinks the data area, so raising fat_sectors until it suffices converges.
    if (total <= kReserved + 2) return kErrNotSupported;
    uint32_t fat_sectors = 1;
    uint64_t clusters = 0;
    for (;;) {
        if (kReserved + uint64_t(kFats) * fat_sectors >= total) return kErrNotSupported;
        clusters = (total - kReserved - uint64_t(kFats) * fat_sectors) / spc;
        uint32_t need = uint32_t(((clusters + 2) * 4 + bps - 1) / bps);
        if (need <= fat_sectors) break;
        fat_sectors = need;
    }
    if (clusters < kMinClusters) return kErrNotSupported;  // too small for FAT32

    auto* b = static_cast<uint8_t*>(heap::alloc_zeroed(bps));
    auto* zero = static_cast<uint8_t*>(heap::alloc_zeroed(64 * 1024));
    if (!b || !zero) {
        heap::free(b);
        heap::free(zero);
        return kErrNoMemory;
    }
    auto put = [&](uint64_t sector, const void* data, uint64_t bytes) {
        return block::write_bytes(uint32_t(index), sector * bps, data, bytes) == int64_t(bytes) ? 0 : kErrIo;
    };
    int64_t r = 0;
    // Reserved area and both FATs start zeroed.
    uint64_t clear = uint64_t(kReserved + kFats * fat_sectors) * bps;
    for (uint64_t at = 0; at < clear && r == 0; at += 64 * 1024) {
        uint64_t n = clear - at < 64 * 1024 ? clear - at : 64 * 1024;
        r = block::write_bytes(uint32_t(index), at, zero, n) == int64_t(n) ? 0 : kErrIo;
    }

    // Boot sector (+ backup at 6).
    const uint8_t jump[3] = {0xEB, 0x58, 0x90};
    mem::copy(b, jump, 3);
    mem::copy(b + 3, "NEKOTIA ", 8);
    wr16(b + 11, uint16_t(bps));
    b[13] = uint8_t(spc);
    wr16(b + 14, kReserved);
    b[16] = kFats;
    b[21] = 0xF8;              // fixed disk
    wr16(b + 24, 32);          // sectors per track, heads: only for old BIOSes
    wr16(b + 26, 64);
    wr32(b + 32, uint32_t(total));
    wr32(b + 36, fat_sectors);
    wr32(b + 44, 2);           // root directory cluster
    wr16(b + 48, 1);           // FSInfo sector
    wr16(b + 50, 6);           // backup boot sector
    b[64] = 0x80;
    b[66] = 0x29;
    wr32(b + 67, uint32_t(cpu::read_tsc()));  // volume serial
    uint8_t name11[11];
    mem::set(name11, ' ', 11);
    if (!label || !*label) label = "NEKOTIA";
    for (uint32_t i = 0; i < 11 && label[i]; ++i) name11[i] = uint8_t(upper(label[i]));
    mem::copy(b + 71, name11, 11);
    mem::copy(b + 82, "FAT32   ", 8);
    b[90] = 0xCD;  // boot code: int 0x18 ("no bootable disk")
    b[91] = 0x18;
    b[510] = 0x55;
    b[511] = 0xAA;
    if (r == 0) r = put(0, b, bps);
    if (r == 0) r = put(6, b, bps);

    // FSInfo (+ backup at 7).
    mem::set(b, 0, bps);
    wr32(b, 0x41615252);
    wr32(b + 484, 0x61417272);
    wr32(b + 488, uint32_t(clusters - 1));  // the root directory uses one
    wr32(b + 492, 3);
    wr32(b + 508, 0xAA550000);
    if (r == 0) r = put(1, b, bps);
    if (r == 0) r = put(7, b, bps);

    // FAT[0] = media, FAT[1] = end marker, FAT[2] = root directory (one cluster) — in both copies.
    mem::set(b, 0, bps);
    wr32(b, 0x0FFFFFF8);
    wr32(b + 4, 0x0FFFFFFF);
    wr32(b + 8, kEndMark);
    for (uint32_t i = 0; i < kFats && r == 0; ++i) r = put(kReserved + uint64_t(i) * fat_sectors, b, bps);

    // Root directory: zeroed, with the volume label entry.
    uint64_t root = kReserved + uint64_t(kFats) * fat_sectors;
    for (uint32_t i = 0; i < spc && r == 0; ++i) r = put(root + i, zero, bps);
    mem::set(b, 0, 32);
    mem::copy(b, name11, 11);
    b[11] = kAttrVolume;
    wr16(b + 24, kDate);
    if (r == 0) r = put(root, b, 32);  // byte-addressed: the block layer merges it into the sector
    if (r == 0) r = block::flush(uint32_t(index));
    heap::free(b);
    heap::free(zero);
    return r;
}

}  // namespace neko::fat
