// Block device registry, byte-addressed access on top of sectors, /dev nodes and RAM disks
// (see neko/block.hpp).
#include <neko/block.hpp>
#include <neko/process.hpp>
#include <neko/runtime.hpp>
#include <neko/vfs.hpp>

namespace neko::block {
namespace {

using namespace proc;  // error codes

struct Device {
    Info info;
    const Ops* ops;
    void* context;
    bool cached;
};

Device g_devices[kMaxDevices];
uint32_t g_count = 0;
sync::SpinLock g_lock;

Device* get(uint32_t index) { return index < __atomic_load_n(&g_count, __ATOMIC_ACQUIRE) ? &g_devices[index] : nullptr; }

bool in_range(const Device* d, uint64_t lba, uint32_t count) {
    return lba <= d->info.sectors && count <= d->info.sectors - lba;
}

// ---- /dev/NAME

int64_t dev_read(void* context, uint64_t offset, void* buffer, uint64_t length) {
    return read_bytes(uint32_t(reinterpret_cast<uintptr_t>(context)), offset, buffer, length);
}

int64_t dev_write(void* context, uint64_t offset, const void* buffer, uint64_t length) {
    return write_bytes(uint32_t(reinterpret_cast<uintptr_t>(context)), offset, buffer, length);
}

uint64_t dev_size(void* context) {
    Device* d = get(uint32_t(reinterpret_cast<uintptr_t>(context)));
    return d ? d->info.sectors * d->info.sector_size : 0;
}

const vfs::DeviceOps kDevOps = {dev_read, dev_write, dev_size, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};

// ---- RAM disks

struct RamDisk {
    uint8_t* data;
    uint32_t sector_size;
};

int64_t ram_read(void* context, uint64_t lba, uint32_t count, void* buffer) {
    auto* r = static_cast<RamDisk*>(context);
    mem::copy(buffer, r->data + lba * r->sector_size, uint64_t(count) * r->sector_size);
    return 0;
}

int64_t ram_write(void* context, uint64_t lba, uint32_t count, const void* buffer) {
    auto* r = static_cast<RamDisk*>(context);
    mem::copy(r->data + lba * r->sector_size, buffer, uint64_t(count) * r->sector_size);
    return 0;
}

const Ops kRamOps = {ram_read, ram_write, nullptr};

int64_t gone_read(void*, uint64_t, uint32_t, void*) { return kErrIo; }
int64_t gone_write(void*, uint64_t, uint32_t, const void*) { return kErrIo; }
const Ops kGoneOps = {gone_read, gone_write, nullptr};

Hotplug g_hotplug = nullptr;

// ---------------------------------------------------------------- the cache

constexpr uint32_t kLine = 4096;
constexpr uint32_t kMaxLines = 2048;  // 8 MiB
constexpr uint32_t kHash = 1024;
constexpr uint32_t kRun = 16;         // lines per device request (64 KiB)

struct Line {
    uint32_t dev;
    uint64_t number;
    uint8_t* data;
    bool dirty;
    uint64_t dirty_since;
    Line* hash_next;
    Line* prev;  // LRU: head is the most recently used
    Line* next;
};

sync::Mutex g_cache;  // every line, and the device I/O the cache does
Line* g_hash[kHash];
Line* g_head = nullptr;
Line* g_tail = nullptr;
uint32_t g_lines = 0, g_capacity = kMaxLines;
uint64_t g_dirty = 0, g_hits = 0, g_misses = 0, g_written = 0;
uint8_t* g_read_run = nullptr;   // kRun lines: reads of missing lines
uint8_t* g_write_run = nullptr;  // kRun lines: write-back
Line** g_sorted = nullptr;       // kMaxLines: dirty lines of one device in order
Line* g_pool = nullptr;          // kMaxLines descriptors, allocated once (their pages come as needed)

uint32_t bucket(uint32_t dev, uint64_t n) { return uint32_t((n * 2654435761u) ^ (dev * 40503u)) % kHash; }

Line* find_line(uint32_t dev, uint64_t n) {
    for (Line* l = g_hash[bucket(dev, n)]; l; l = l->hash_next)
        if (l->dev == dev && l->number == n) return l;
    return nullptr;
}

void lru_unlink(Line* l) {
    if (l->prev) l->prev->next = l->next;
    else g_head = l->next;
    if (l->next) l->next->prev = l->prev;
    else g_tail = l->prev;
    l->prev = l->next = nullptr;
}

void lru_front(Line* l) {
    l->next = g_head;
    l->prev = nullptr;
    if (g_head) g_head->prev = l;
    g_head = l;
    if (!g_tail) g_tail = l;
}

void touch(Line* l) {
    if (g_head == l) return;
    lru_unlink(l);
    lru_front(l);
}

void unhash(Line* l) {
    for (Line** p = &g_hash[bucket(l->dev, l->number)]; *p; p = &(*p)->hash_next) {
        if (*p == l) {
            *p = l->hash_next;
            return;
        }
    }
}

void install(Line* l, uint32_t dev, uint64_t n) {
    l->dev = dev;
    l->number = n;
    l->dirty = false;
    Line*& head = g_hash[bucket(dev, n)];
    l->hash_next = head;
    head = l;
    lru_front(l);
}

uint32_t sectors_per_line(const Device* d) { return kLine / d->info.sector_size; }

// Sectors of line `n` that exist (the last line of a disk may be short).
uint32_t line_sectors(const Device* d, uint64_t n) {
    uint64_t first = n * sectors_per_line(d);
    uint64_t left = d->info.sectors - first;
    return left < sectors_per_line(d) ? uint32_t(left) : sectors_per_line(d);
}

// Writes back every dirty line of device `dev` (in order, neighbouring lines in one request). g_cache held.
int64_t write_back(uint32_t dev) {
    Device* d = &g_devices[dev];
    uint32_t n = 0;
    for (Line* l = g_head; l; l = l->next)
        if (l->dev == dev && l->dirty) g_sorted[n++] = l;
    // shell sort by line number
    for (uint32_t gap = n / 2; gap > 0; gap /= 2)
        for (uint32_t i = gap; i < n; ++i)
            for (uint32_t j = i; j >= gap && g_sorted[j - gap]->number > g_sorted[j]->number; j -= gap) {
                Line* t = g_sorted[j];
                g_sorted[j] = g_sorted[j - gap];
                g_sorted[j - gap] = t;
            }
    int64_t error = 0;
    for (uint32_t i = 0; i < n;) {
        uint32_t run = 1;
        while (i + run < n && run < kRun && g_sorted[i + run]->number == g_sorted[i]->number + run) ++run;
        uint32_t sectors = 0;
        for (uint32_t k = 0; k < run; ++k) {
            mem::copy(g_write_run + k * kLine, g_sorted[i + k]->data, kLine);
            sectors += line_sectors(d, g_sorted[i + k]->number);
        }
        int64_t r = d->ops->write(d->context, g_sorted[i]->number * sectors_per_line(d), sectors, g_write_run);
        if (r < 0) error = r;   // the lines stay dirty: tried again later
        else {
            for (uint32_t k = 0; k < run; ++k) {
                g_sorted[i + k]->dirty = false;
                --g_dirty;
            }
            g_written += run;
            __atomic_add_fetch(&d->info.written_sectors, sectors, __ATOMIC_RELAXED);
        }
        i += run;
    }
    return error;
}

// A line to fill: a new one while there is room, else the least recently used clean one (writing dirty lines
// back first when every line is dirty). g_cache held.
Line* take_line() {
    if (g_lines < g_capacity) {
        Line* l = &g_pool[g_lines];
        if (!l->data) l->data = static_cast<uint8_t*>(pmm::alloc_page());
        if (l->data) {
            ++g_lines;
            return l;
        }
        g_capacity = g_lines;  // out of memory: this is as big as the cache gets
    }
    for (int round = 0; round < 2; ++round) {
        for (Line* l = g_tail; l; l = l->prev) {
            if (l->dirty) continue;
            lru_unlink(l);
            unhash(l);
            return l;
        }
        if (g_tail) write_back(g_tail->dev);  // all dirty: make room
    }
    return nullptr;
}

// Forgets every line of `dev` (they stay in the LRU list as unhashed, clean lines of no device: reused first).
void drop_lines(uint32_t dev) {
    for (Line* l = g_head; l;) {
        Line* next = l->next;
        if (l->dev == dev) {
            if (l->dirty) --g_dirty;
            l->dirty = false;
            unhash(l);
            l->dev = ~0u;
            lru_unlink(l);
            l->next = nullptr;
            l->prev = g_tail;   // to the cold end
            if (g_tail) g_tail->next = l;
            else g_head = l;
            g_tail = l;
            Line*& head = g_hash[bucket(~0u, l->number)];
            l->hash_next = head;   // hashed under no device, so take_line's unhash finds it
            head = l;
        }
        l = next;
    }
}

// The part of line `line` (its first sector `first`) that request [lba, lba + n) covers, copied either way.
void copy_part(Device* d, Line* line, uint64_t lba, uint32_t n, uint8_t* buffer, bool into_line) {
    uint64_t first = line->number * sectors_per_line(d);
    uint64_t from = lba > first ? lba : first;
    uint64_t to = lba + n < first + sectors_per_line(d) ? lba + n : first + sectors_per_line(d);
    uint64_t ss = d->info.sector_size;
    uint8_t* in_line = line->data + (from - first) * ss;
    uint8_t* in_buffer = buffer + (from - lba) * ss;
    if (into_line) mem::copy(in_line, in_buffer, (to - from) * ss);
    else mem::copy(in_buffer, in_line, (to - from) * ss);
}

int64_t cached_read(uint32_t dev, uint64_t lba, uint32_t n, uint8_t* buffer) {
    Device* d = &g_devices[dev];
    sync::MutexGuard guard(g_cache);
    uint64_t last = (lba + n - 1) / sectors_per_line(d);
    for (uint64_t ln = lba / sectors_per_line(d); ln <= last;) {
        if (Line* l = find_line(dev, ln)) {
            ++g_hits;
            copy_part(d, l, lba, n, buffer, false);
            touch(l);
            ++ln;
            continue;
        }
        uint64_t end = ln + 1;  // a run of missing lines, read at once
        while (end <= last && end - ln < kRun && !find_line(dev, end)) ++end;
        uint32_t sectors = 0;
        for (uint64_t k = ln; k < end; ++k) sectors += line_sectors(d, k);
        int64_t r = d->ops->read(d->context, ln * sectors_per_line(d), sectors, g_read_run);
        if (r < 0) return r;
        for (uint64_t k = ln; k < end; ++k) {
            Line* l = take_line();
            if (!l) return kErrNoMemory;
            mem::copy(l->data, g_read_run + (k - ln) * kLine, kLine);
            install(l, dev, k);
            copy_part(d, l, lba, n, buffer, false);
            ++g_misses;
        }
        ln = end;
    }
    return 0;
}

int64_t cached_write(uint32_t dev, uint64_t lba, uint32_t n, const uint8_t* buffer) {
    Device* d = &g_devices[dev];
    sync::MutexGuard guard(g_cache);
    uint64_t last = (lba + n - 1) / sectors_per_line(d);
    for (uint64_t ln = lba / sectors_per_line(d); ln <= last; ++ln) {
        Line* l = find_line(dev, ln);
        if (!l) {
            l = take_line();
            if (!l) return kErrNoMemory;
            uint64_t first = ln * sectors_per_line(d);
            bool whole = lba <= first && lba + n >= first + line_sectors(d, ln);
            if (!whole) {  // the rest of the line must come from the disk
                int64_t r = d->ops->read(d->context, first, line_sectors(d, ln), l->data);
                if (r < 0) {
                    install(l, ~0u, ln);   // kept as a line of no device: reused first
                    return r;
                }
            }
            install(l, dev, ln);
        } else {
            touch(l);
        }
        copy_part(d, l, lba, n, const_cast<uint8_t*>(buffer), true);
        if (!l->dirty) {
            l->dirty = true;
            l->dirty_since = time::uptime_ms();
            ++g_dirty;
        }
    }
    if (g_dirty > g_capacity / 2) write_back(dev);  // a lot is waiting: do not let it pile up
    return 0;
}

void writeback_thread(uint64_t) {
    for (;;) {
        sched::sleep_ms(1000);
        sync::MutexGuard guard(g_cache);
        if (!g_dirty) continue;
        uint64_t now = time::uptime_ms();
        bool old[kMaxDevices] = {};
        for (Line* l = g_head; l; l = l->next)
            if (l->dirty && now - l->dirty_since >= 1000 && l->dev < kMaxDevices) old[l->dev] = true;
        for (uint32_t i = 0; i < kMaxDevices; ++i)
            if (old[i]) write_back(i);
    }
}

bool cache_ready() {
    if (g_sorted) return true;
    g_read_run = static_cast<uint8_t*>(heap::alloc(kRun * kLine));
    g_write_run = static_cast<uint8_t*>(heap::alloc(kRun * kLine));
    g_sorted = static_cast<Line**>(heap::alloc(sizeof(Line*) * kMaxLines));
    g_pool = static_cast<Line*>(heap::alloc_zeroed(sizeof(Line) * kMaxLines));
    return g_read_run && g_write_run && g_sorted && g_pool;
}

}  // namespace

void start_writeback() { sched::spawn("block-writeback", &writeback_thread, 0, sched::Priority::Low); }

CacheStats cache_stats() {
    return {g_lines, g_dirty, g_capacity, g_hits, g_misses, g_written};
}

int64_t sync_all() {
    int64_t error = 0;
    for (uint32_t i = 0; i < count(); ++i) {
        int64_t r = flush(i);
        if (r < 0 && r != kErrNotFound) error = r;
    }
    return error;
}

void set_hotplug(Hotplug handler) { __atomic_store_n(&g_hotplug, handler, __ATOMIC_RELEASE); }

void remove(uint32_t index) {
    char path[32] = "/dev/";
    {
        sync::ScopedLock guard(g_lock);
        Device* d = get(index);
        if (!d || d->info.removed) return;
        d->info.removed = true;
        d->cached = false;
        str::copy(path + 5, reinterpret_cast<const char*>(d->info.name), sizeof(path) - 5);
    }
    {
        sync::MutexGuard cache(g_cache);  // what was not written back is lost with the device
        drop_lines(index);
        sync::ScopedLock guard(g_lock);
        g_devices[index].ops = &kGoneOps;
        g_devices[index].context = nullptr;
    }
    vfs::remove(path);
    if (Hotplug h = __atomic_load_n(&g_hotplug, __ATOMIC_ACQUIRE)) h(index, false);
}

int32_t add(const char* name, const char* model, uint64_t sectors, uint32_t sector_size, bool read_only,
            const Ops* ops, void* context) {
    if (!name || !*name || !ops || !ops->read || sector_size < 512 || (sector_size & (sector_size - 1))) return kErrBadArgument;
    if (str::length(name) >= sizeof(Info::name)) return kErrNameTooLong;
    uint32_t index;
    {
        sync::ScopedLock guard(g_lock);
        // a removed device's slot with the same name is reused (the stick came back), else a new one
        index = g_count;
        for (uint32_t i = 0; i < g_count; ++i)
            if (g_devices[i].info.removed && str::equals(reinterpret_cast<const char*>(g_devices[i].info.name), name)) index = i;
        for (uint32_t i = 0; i < g_count; ++i)
            if (str::equals(reinterpret_cast<const char*>(g_devices[i].info.name), name) && !g_devices[i].info.removed)
                return kErrExists;
        if (index == kMaxDevices) return kErrLimit;
        Device& d = g_devices[index];
        mem::set(&d, 0, sizeof(d));
        str::copy(reinterpret_cast<char*>(d.info.name), name, sizeof(d.info.name));
        str::copy(reinterpret_cast<char*>(d.info.model), model ? model : "", sizeof(d.info.model));
        d.info.sectors = sectors;
        d.info.sector_size = sector_size;
        d.info.read_only = read_only || !ops->write;
        d.ops = ops;
        d.context = context;
        d.cached = ops != &kRamOps && sector_size <= kLine && cache_ready();
        if (index == g_count) __atomic_store_n(&g_count, index + 1, __ATOMIC_RELEASE);
    }
    char path[32] = "/dev/";
    str::copy(path + 5, name, sizeof(path) - 5);
    vfs::create_device(path, &kDevOps, reinterpret_cast<void*>(uintptr_t(index)));
    if (Hotplug h = __atomic_load_n(&g_hotplug, __ATOMIC_ACQUIRE)) h(index, true);
    return int32_t(index);
}

int32_t add_ramdisk(const char* name, uint64_t bytes) {
    constexpr uint32_t kSector = 512;
    if (bytes < kSector) return kErrBadArgument;
    auto* r = static_cast<RamDisk*>(heap::alloc(sizeof(RamDisk)));
    uint8_t* data = static_cast<uint8_t*>(heap::alloc_zeroed(bytes));
    if (!r || !data) {
        heap::free(r);
        heap::free(data);
        return kErrNoMemory;
    }
    r->data = data;
    r->sector_size = kSector;
    int32_t index = add(name, "RAM disk", bytes / kSector, kSector, false, &kRamOps, r);
    if (index < 0) {
        heap::free(data);
        heap::free(r);
    }
    return index;
}

uint32_t count() { return __atomic_load_n(&g_count, __ATOMIC_ACQUIRE); }

bool info(uint32_t index, Info& out) {
    Device* d = get(index);
    if (!d) return false;
    out = d->info;
    return true;
}

int32_t find(const char* name) {
    for (uint32_t i = 0; i < count(); ++i)
        if (!g_devices[i].info.removed && str::equals(reinterpret_cast<const char*>(g_devices[i].info.name), name))
            return int32_t(i);
    return -1;
}

int64_t read(uint32_t index, uint64_t lba, uint32_t n, void* buffer) {
    Device* d = get(index);
    if (!d) return kErrNotFound;
    if (!in_range(d, lba, n)) return kErrBadArgument;
    if (n == 0) return 0;
    int64_t r = d->cached ? cached_read(index, lba, n, static_cast<uint8_t*>(buffer)) : d->ops->read(d->context, lba, n, buffer);
    if (r == 0) __atomic_add_fetch(&d->info.read_sectors, n, __ATOMIC_RELAXED);
    return r;
}

int64_t write(uint32_t index, uint64_t lba, uint32_t n, const void* buffer) {
    Device* d = get(index);
    if (!d) return kErrNotFound;
    if (d->info.read_only) return kErrAccess;
    if (!in_range(d, lba, n)) return kErrBadArgument;
    if (n == 0) return 0;
    if (d->cached) return cached_write(index, lba, n, static_cast<const uint8_t*>(buffer));  // counted when written back
    int64_t r = d->ops->write(d->context, lba, n, buffer);
    if (r == 0) __atomic_add_fetch(&d->info.written_sectors, n, __ATOMIC_RELAXED);
    return r;
}

int64_t flush(uint32_t index) {
    Device* d = get(index);
    if (!d || d->info.removed) return kErrNotFound;
    if (d->cached) {
        sync::MutexGuard guard(g_cache);
        int64_t r = write_back(index);
        if (r < 0) return r;
    }
    return d->ops->flush ? d->ops->flush(d->context) : 0;
}

namespace {

// Byte range → whole sectors straight to/from the caller's buffer, partial ones through `bounce`.
int64_t transfer(uint32_t index, uint64_t offset, uint8_t* buffer, uint64_t length, bool writing) {
    Device* d = get(index);
    if (!d) return kErrNotFound;
    if (writing && d->info.read_only) return kErrAccess;
    uint64_t ss = d->info.sector_size;
    uint64_t size = d->info.sectors * ss;
    if (offset >= size || length == 0) return 0;
    if (length > size - offset) length = size - offset;

    uint8_t* bounce = nullptr;
    uint64_t done = 0;
    int64_t error = 0;
    while (done < length) {
        uint64_t at = offset + done;
        uint64_t lba = at / ss;
        uint64_t in_sector = at % ss;
        uint64_t left = length - done;
        if (in_sector == 0 && left >= ss) {  // whole sectors (up to 1 MiB per call)
            uint64_t n = left / ss;
            if (n > (1u << 20) / ss) n = (1u << 20) / ss;
            error = writing ? write(index, lba, uint32_t(n), buffer + done) : read(index, lba, uint32_t(n), buffer + done);
            if (error) break;
            done += n * ss;
            continue;
        }
        if (!bounce && !(bounce = static_cast<uint8_t*>(heap::alloc(ss)))) {
            error = kErrNoMemory;
            break;
        }
        uint64_t chunk = ss - in_sector < left ? ss - in_sector : left;
        error = read(index, lba, 1, bounce);
        if (error) break;
        if (writing) {
            mem::copy(bounce + in_sector, buffer + done, chunk);
            error = write(index, lba, 1, bounce);
            if (error) break;
        } else {
            mem::copy(buffer + done, bounce + in_sector, chunk);
        }
        done += chunk;
    }
    heap::free(bounce);
    return done ? int64_t(done) : error;
}

}  // namespace

int64_t read_bytes(uint32_t index, uint64_t offset, void* buffer, uint64_t length) {
    return transfer(index, offset, static_cast<uint8_t*>(buffer), length, false);
}

int64_t write_bytes(uint32_t index, uint64_t offset, const void* buffer, uint64_t length) {
    return transfer(index, offset, static_cast<uint8_t*>(const_cast<void*>(buffer)), length, true);
}

}  // namespace neko::block
