// ext2 / ext3 / ext4, read-only (fs/ext2.cpp): mount a Linux partition and read its files.
//
//   superblock (byte 1024) → block group descriptors → inode tables → inodes → data blocks
//
// Data blocks are found through the classic block map (12 direct, single / double / triple indirect: ext2,
// ext3) or an extent tree (ext4, inode flag EXTENTS); 64-bit group descriptors (ext4 "64bit") are understood.
// The journal is not replayed: a volume that was not cleanly unmounted shows its last committed state minus
// what only the journal has. Like the FAT driver, the whole directory tree becomes VFS nodes at mount time;
// file contents are read on demand (through the block cache), neighbouring blocks in one request. Symbolic
// links, devices and sockets are skipped. Every change fails with kErrAccess.
#include <neko/ext2.hpp>
#include <neko/process.hpp>
#include <neko/runtime.hpp>
#include <neko/vfs.hpp>

namespace neko::ext2 {
namespace {

using namespace proc;  // error codes

constexpr uint16_t kMagic = 0xEF53;
constexpr uint32_t kIncompatCompression = 0x1, kIncompatFiletype = 0x2, kIncompatRecover = 0x4,
                   kIncompatJournalDev = 0x8, kIncompatMetaBg = 0x10, kIncompatExtents = 0x40, kIncompat64Bit = 0x80,
                   kIncompatFlexBg = 0x200, kIncompatInlineData = 0x8000;
constexpr uint32_t kSupported = kIncompatFiletype | kIncompatRecover | kIncompatExtents | kIncompat64Bit | kIncompatFlexBg |
                                0x2000 /* csum seed */ | 0x400 /* ea_inode */ | 0x200;
constexpr uint32_t kFlagExtents = 0x80000;
constexpr uint16_t kModeDir = 0x4000, kModeFile = 0x8000, kModeTypeMask = 0xF000;

uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

struct Volume {
    uint32_t disk;
    uint32_t block_size;
    uint64_t blocks, free_blocks;
    uint32_t inodes_per_group, inode_size, desc_size, groups;
    uint64_t descriptors;      // block of the group descriptor table
    bool extents;
    uint8_t* descs;            // the whole descriptor table
    uint8_t* scratch;          // one block (inode and directory reading)
    uint8_t* node;             // one block: an extent tree node below the inode
    uint32_t nodes;            // VFS nodes this mount made (a crafted volume must not exhaust memory)
    uint32_t path[40];         // directory inodes from the root to the one being scanned (cycles)
    sync::Mutex lock;
    char name[16];
    char type[8];
};

// What a node remembers: its inode, read once at mount.
struct Entry {
    uint32_t ino;
    uint16_t mode;
    uint32_t flags;
    uint32_t block[15];        // i_block: block map or extent tree root
    uint64_t size;
};

int64_t disk(Volume* v, uint64_t block, void* out, uint64_t bytes) {
    int64_t n = block::read_bytes(v->disk, block * v->block_size, out, bytes);
    return n == int64_t(bytes) ? 0 : kErrIo;
}

uint64_t inode_table(Volume* v, uint32_t group) {
    const uint8_t* d = v->descs + uint64_t(group) * v->desc_size;
    uint64_t lo = rd32(d + 8);
    uint64_t hi = v->desc_size >= 64 ? rd32(d + 0x28) : 0;
    return lo | (hi << 32);
}

int64_t read_inode(Volume* v, uint32_t ino, Entry& e) {
    if (ino == 0) return kErrBadArgument;
    uint32_t group = (ino - 1) / v->inodes_per_group, index = (ino - 1) % v->inodes_per_group;
    if (group >= v->groups) return kErrBadArgument;
    uint8_t raw[256];
    uint32_t size = v->inode_size < sizeof(raw) ? v->inode_size : uint32_t(sizeof(raw));
    uint64_t at = inode_table(v, group) * v->block_size + uint64_t(index) * v->inode_size;
    if (block::read_bytes(v->disk, at, raw, size) != int64_t(size)) return kErrIo;
    e.ino = ino;
    e.mode = rd16(raw);
    e.flags = rd32(raw + 32);
    for (uint32_t i = 0; i < 15; ++i) e.block[i] = rd32(raw + 40 + 4 * i);
    e.size = rd32(raw + 4) | (uint64_t(rd32(raw + 108)) << 32);
    return 0;
}

// Physical block of logical block `n` of the file (0 = a hole), or an Error.
int64_t indirect(Volume* v, uint32_t table, uint64_t index) {
    if (!table) return 0;
    uint8_t entry[4];
    if (block::read_bytes(v->disk, uint64_t(table) * v->block_size + index * 4, entry, 4) != 4) return kErrIo;
    return rd32(entry);
}

int64_t map_classic(Volume* v, const Entry& e, uint64_t n) {
    uint64_t per = v->block_size / 4;
    if (n < 12) return e.block[n];
    n -= 12;
    if (n < per) return indirect(v, e.block[12], n);
    n -= per;
    if (n < per * per) {
        int64_t mid = indirect(v, e.block[13], n / per);
        return mid <= 0 ? mid : indirect(v, uint32_t(mid), n % per);
    }
    n -= per * per;
    if (n < per * per * per) {
        int64_t a = indirect(v, e.block[14], n / (per * per));
        if (a <= 0) return a;
        int64_t b = indirect(v, uint32_t(a), (n / per) % per);
        return b <= 0 ? b : indirect(v, uint32_t(b), n % per);
    }
    return kErrBadArgument;
}

// ext4 extent tree: header (magic 0xF30A, entries, depth), then 12-byte entries — leaves map a run of
// logical blocks to physical ones, index nodes point at the next level.
int64_t map_extents(Volume* v, const Entry& e, uint64_t n, uint64_t* run) {
    const uint8_t* h = reinterpret_cast<const uint8_t*>(e.block);
    uint32_t room = sizeof(e.block);  // bytes of the node h points into: the inode's 60, then a block
    for (int level = 0; level < 8; ++level) {
        if (rd16(h) != 0xF30A) return kErrIo;
        uint16_t entries = rd16(h + 2), max = rd16(h + 4), depth = rd16(h + 6);
        if (entries > max || 12u + 12u * entries > room) return kErrIo;  // a header that lies about its size
        const uint8_t* best = nullptr;
        for (uint16_t i = 0; i < entries; ++i) {
            const uint8_t* x = h + 12 + 12 * i;
            if (rd32(x) <= n) best = x;  // entries are sorted by first logical block
            else break;
        }
        if (!best) return 0;  // a hole before the first extent
        if (depth == 0) {
            uint32_t first = rd32(best);
            uint16_t length = rd16(best + 4);
            if (length > 32768) length = uint16_t(length - 32768);  // uninitialised extent: reads as zeros
            uint64_t start = (uint64_t(rd16(best + 6)) << 32) | rd32(best + 8);
            if (n >= first + length) return 0;
            if (run) *run = first + length - n;
            if (rd16(best + 4) > 32768) return 0;
            return int64_t(start + (n - first));
        }
        uint64_t child = (uint64_t(rd16(best + 8)) << 32) | rd32(best + 4);
        if (child >= v->blocks || disk(v, child, v->node, v->block_size) < 0) return kErrIo;
        h = v->node;
        room = v->block_size;
    }
    return kErrIo;
}

int64_t map(Volume* v, const Entry& e, uint64_t n, uint64_t* run = nullptr) {
    if (run) *run = 1;
    return (e.flags & kFlagExtents) ? map_extents(v, e, n, run) : map_classic(v, e, n);
}

// Reads [offset, offset + length) of a file; neighbouring physical blocks go in one request.
int64_t read_file(Volume* v, const Entry& e, uint64_t offset, uint8_t* out, uint64_t length) {
    if (offset >= e.size) return 0;
    if (length > e.size - offset) length = e.size - offset;
    uint64_t done = 0;
    while (done < length) {
        uint64_t at = offset + done;
        uint64_t logical = at / v->block_size, within = at % v->block_size;
        uint64_t run = 1;
        int64_t physical = map(v, e, logical, &run);
        if (physical < 0) return done ? int64_t(done) : physical;
        uint64_t chunk = v->block_size - within;
        if (chunk > length - done) chunk = length - done;
        if (physical == 0) {
            mem::set(out + done, 0, chunk);  // a hole
            done += chunk;
            continue;
        }
        // Extend over blocks that follow on disk (from the extent, or by asking the block map), ≤ 256 KiB.
        uint64_t blocks = 1;
        while (chunk < length - done && chunk < 256 * 1024) {
            int64_t next;
            if (blocks < run) next = physical + int64_t(blocks);
            else {
                uint64_t more = 1;
                next = map(v, e, logical + blocks, &more);
            }
            if (next != physical + int64_t(blocks)) break;
            uint64_t add = v->block_size;
            if (add > length - done - chunk) add = length - done - chunk;
            chunk += add;
            ++blocks;
        }
        if (block::read_bytes(v->disk, uint64_t(physical) * v->block_size + within, out + done, chunk) != int64_t(chunk))
            return done ? int64_t(done) : kErrIo;
        done += chunk;
    }
    return int64_t(done);
}

constexpr uint32_t kMaxDepth = 32;
constexpr uint32_t kMaxNodes = 200000;

struct Sub {
    vfs::Node* node;
    Entry* entry;
};

// Adds the entries of directory `dir_entry` under `dir`, then descends. A directory whose inode is already on
// the path from the root (a crafted or broken volume looping back) is listed but not entered, and one mount
// makes at most kMaxNodes nodes; the pending subdirectories live on the heap (the stack is 32 KiB).
int64_t scan(Volume* v, vfs::Node* dir, const Entry& dir_entry, uint32_t depth) {
    if (depth >= kMaxDepth) return 0;
    v->path[depth] = dir_entry.ino;
    auto* subs = static_cast<Sub*>(heap::alloc(sizeof(Sub) * 32));
    if (!subs) return kErrNoMemory;
    uint32_t nsubs = 0;
    int64_t error = 0;
    for (uint64_t at = 0; at < dir_entry.size && error == 0; at += v->block_size) {
        int64_t got = read_file(v, dir_entry, at, v->scratch, v->block_size);
        if (got <= 0) {
            if (got < 0) error = got;
            break;
        }
        for (uint32_t off = 0; off + 8 <= uint32_t(got);) {
            const uint8_t* d = v->scratch + off;
            uint32_t ino = rd32(d);
            uint16_t rec = rd16(d + 4);
            uint8_t name_length = d[6];
            if (rec < 8 || off + rec > uint32_t(got)) break;
            off += rec;
            if (!ino || name_length == 0 || name_length > vfs::kMaxName || 8u + name_length > rec) continue;
            char name[vfs::kMaxName + 1];
            mem::copy(name, d + 8, name_length);
            name[name_length] = 0;
            if ((name_length == 1 && name[0] == '.') || (name_length == 2 && name[0] == '.' && name[1] == '.')) continue;
            if (vfs::has_child(dir, name)) continue;
            if (++v->nodes > kMaxNodes) {
                error = kErrLimit;
                break;
            }
            auto* e = static_cast<Entry*>(heap::alloc_zeroed(sizeof(Entry)));
            if (!e) {
                error = kErrNoMemory;
                break;
            }
            uint8_t* keep = v->scratch;  // read_inode does not touch scratch, but descending does
            if (read_inode(v, ino, *e) < 0) {
                heap::free(e);
                continue;
            }
            uint16_t kind = e->mode & kModeTypeMask;
            if (kind != kModeDir && kind != kModeFile) {  // symlinks, devices, FIFOs, sockets
                heap::free(e);
                continue;
            }
            bool is_dir = kind == kModeDir;
            vfs::Node* n = vfs::add_node(dir, name, is_dir ? vfs::Type::Directory : vfs::Type::File, is_dir ? 0 : e->size, e, false);
            if (!n) {
                heap::free(e);
                error = kErrNoMemory;
                break;
            }
            bool loop = false;
            for (uint32_t k = 0; k <= depth; ++k) loop |= v->path[k] == ino;
            if (is_dir && !loop) {
                if (nsubs == 32) {  // descend now (the scratch block is reread below)
                    for (uint32_t i = 0; i < nsubs && error == 0; ++i) error = scan(v, subs[i].node, *subs[i].entry, depth + 1);
                    nsubs = 0;
                    v->path[depth] = dir_entry.ino;
                    if (error == 0 && read_file(v, dir_entry, at, keep, v->block_size) < 0) error = kErrIo;
                    if (error) break;
                }
                subs[nsubs++] = {n, e};
            }
        }
    }
    for (uint32_t i = 0; i < nsubs && error == 0; ++i) error = scan(v, subs[i].node, *subs[i].entry, depth + 1);
    heap::free(subs);
    return error;
}

// ---------------------------------------------------------------- FsOps

int64_t op_read(void* fs, vfs::Node* n, uint64_t offset, void* buffer, uint64_t length) {
    auto* v = static_cast<Volume*>(fs);
    auto* e = static_cast<Entry*>(vfs::node_data(n));
    if (!e) return kErrIo;
    sync::MutexGuard guard(v->lock);
    return read_file(v, *e, offset, static_cast<uint8_t*>(buffer), length);
}

int64_t op_write(void*, vfs::Node*, uint64_t, const void*, uint64_t) { return kErrAccess; }
int64_t op_truncate(void*, vfs::Node*, uint64_t) { return kErrAccess; }
int64_t op_create(void*, vfs::Node*, const char*, vfs::Type, vfs::Node*&) { return kErrAccess; }
int64_t op_remove(void*, vfs::Node*) { return kErrAccess; }
int64_t op_rename(void*, vfs::Node*, vfs::Node*, const char*) { return kErrAccess; }
int64_t op_sync(void*) { return 0; }
void op_release(void*, void* data) { heap::free(data); }

void free_volume(Volume* v) {
    heap::free(v->descs);
    heap::free(v->scratch);
    heap::free(v->node);
    heap::free(v);
}

void op_unmount(void* fs) { free_volume(static_cast<Volume*>(fs)); }

void op_usage(void* fs, uint64_t& total, uint64_t& free) {
    auto* v = static_cast<Volume*>(fs);
    total = v->blocks * v->block_size;
    free = v->free_blocks * v->block_size;
}

const vfs::FsOps kOps = {"ext2", op_read, op_write, op_truncate, op_create, op_remove, op_rename,
                         op_sync, op_release, op_unmount, op_usage};
const vfs::FsOps kOps4 = {"ext4", op_read, op_write, op_truncate, op_create, op_remove, op_rename,
                          op_sync, op_release, op_unmount, op_usage};

int32_t device_index(const char* device) {
    if (!device) return kErrNotFound;
    if (str::length(device) > 5 && mem::compare(device, "/dev/", 5) == 0) device += 5;
    int32_t i = block::find(device);
    return i < 0 ? kErrNotFound : i;
}

bool read_super(uint32_t index, uint8_t* sb) {
    return block::read_bytes(index, 1024, sb, 1024) == 1024 && rd16(sb + 56) == kMagic;
}

}  // namespace

bool probe(const char* device) {
    int32_t index = device_index(device);
    uint8_t sb[1024];
    return index >= 0 && read_super(uint32_t(index), sb);
}

int64_t mount(const char* device, const char* dir) {
    int32_t index = device_index(device);
    if (index < 0) return index;
    uint8_t sb[1024];
    if (!read_super(uint32_t(index), sb)) return kErrNotSupported;
    uint32_t log = rd32(sb + 24);
    uint32_t rev = rd32(sb + 76);
    uint32_t incompat = rev >= 1 ? rd32(sb + 96) : 0;
    if (log > 6 || (incompat & ~kSupported)) return kErrNotSupported;  // compression, meta_bg, inline data…

    auto* v = static_cast<Volume*>(heap::alloc_zeroed(sizeof(Volume)));
    if (!v) return kErrNoMemory;
    v->disk = uint32_t(index);
    v->block_size = 1024u << log;
    v->blocks = rd32(sb + 4) | ((incompat & kIncompat64Bit) ? uint64_t(rd32(sb + 0x150)) << 32 : 0);
    v->free_blocks = rd32(sb + 12) | ((incompat & kIncompat64Bit) ? uint64_t(rd32(sb + 0x158)) << 32 : 0);
    uint32_t blocks_per_group = rd32(sb + 32);
    v->inodes_per_group = rd32(sb + 40);
    v->inode_size = rev >= 1 ? rd16(sb + 88) : 128;
    v->desc_size = (incompat & kIncompat64Bit) && rd16(sb + 254) >= 32 ? rd16(sb + 254) : 32;
    uint32_t first_data = rd32(sb + 20);
    if (!blocks_per_group || !v->inodes_per_group || v->inode_size < 128) {
        free_volume(v);
        return kErrNotSupported;
    }
    v->groups = uint32_t((v->blocks - first_data + blocks_per_group - 1) / blocks_per_group);
    v->descriptors = first_data + 1;
    v->extents = incompat & kIncompatExtents;
    v->lock.set_name("ext2 volume");
    str::copy(v->type, v->extents ? "ext4" : "ext2", sizeof(v->type));
    block::Info bi;
    block::info(uint32_t(index), bi);
    str::copy(v->name, reinterpret_cast<const char*>(bi.name), sizeof(v->name));
    uint64_t table = uint64_t(v->groups) * v->desc_size;
    v->descs = static_cast<uint8_t*>(heap::alloc(table));
    v->scratch = static_cast<uint8_t*>(heap::alloc(v->block_size));
    v->node = static_cast<uint8_t*>(heap::alloc(v->block_size));
    auto* root = static_cast<Entry*>(heap::alloc_zeroed(sizeof(Entry)));
    if (!v->descs || !v->scratch || !v->node || !root) {
        heap::free(root);
        free_volume(v);
        return kErrNoMemory;
    }
    if (block::read_bytes(v->disk, v->descriptors * v->block_size, v->descs, table) != int64_t(table) ||
        read_inode(v, 2, *root) < 0 || (root->mode & kModeTypeMask) != kModeDir) {
        heap::free(root);
        free_volume(v);
        return kErrIo;
    }
    vfs::Node* node;
    int64_t r = vfs::mount(dir, v->extents ? &kOps4 : &kOps, v, v->name, root, node);
    if (r < 0) {
        heap::free(root);
        free_volume(v);
        return r;
    }
    {
        sync::MutexGuard guard(v->lock);
        r = scan(v, node, *root, 0);
    }
    if (r < 0) vfs::unmount(dir);
    return r;
}

}  // namespace neko::ext2
