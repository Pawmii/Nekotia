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

}  // namespace

int32_t add(const char* name, const char* model, uint64_t sectors, uint32_t sector_size, bool read_only,
            const Ops* ops, void* context) {
    if (!name || !*name || !ops || !ops->read || sector_size < 512 || (sector_size & (sector_size - 1))) return kErrBadArgument;
    if (str::length(name) >= sizeof(Info::name)) return kErrNameTooLong;
    uint32_t index;
    {
        sync::ScopedLock guard(g_lock);
        if (g_count == kMaxDevices) return kErrLimit;
        index = g_count;
        Device& d = g_devices[index];
        mem::set(&d, 0, sizeof(d));
        str::copy(reinterpret_cast<char*>(d.info.name), name, sizeof(d.info.name));
        str::copy(reinterpret_cast<char*>(d.info.model), model ? model : "", sizeof(d.info.model));
        d.info.sectors = sectors;
        d.info.sector_size = sector_size;
        d.info.read_only = read_only || !ops->write;
        d.ops = ops;
        d.context = context;
        __atomic_store_n(&g_count, index + 1, __ATOMIC_RELEASE);
    }
    char path[32] = "/dev/";
    str::copy(path + 5, name, sizeof(path) - 5);
    vfs::create_device(path, &kDevOps, reinterpret_cast<void*>(uintptr_t(index)));
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
        if (str::equals(reinterpret_cast<const char*>(g_devices[i].info.name), name)) return int32_t(i);
    return -1;
}

int64_t read(uint32_t index, uint64_t lba, uint32_t n, void* buffer) {
    Device* d = get(index);
    if (!d) return kErrNotFound;
    if (!in_range(d, lba, n)) return kErrBadArgument;
    if (n == 0) return 0;
    int64_t r = d->ops->read(d->context, lba, n, buffer);
    if (r == 0) __atomic_add_fetch(&d->info.read_sectors, n, __ATOMIC_RELAXED);
    return r;
}

int64_t write(uint32_t index, uint64_t lba, uint32_t n, const void* buffer) {
    Device* d = get(index);
    if (!d) return kErrNotFound;
    if (d->info.read_only) return kErrAccess;
    if (!in_range(d, lba, n)) return kErrBadArgument;
    if (n == 0) return 0;
    int64_t r = d->ops->write(d->context, lba, n, buffer);
    if (r == 0) __atomic_add_fetch(&d->info.written_sectors, n, __ATOMIC_RELAXED);
    return r;
}

int64_t flush(uint32_t index) {
    Device* d = get(index);
    if (!d) return kErrNotFound;
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
