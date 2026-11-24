// Block devices (core/block.cpp): disks addressed in sectors, whatever the driver behind them
// (virtio-blk in drivers/virtio_blk.cpp, RAM disks here). Every device also appears as /dev/NAME,
// a seekable device node with byte offsets (partial sectors are read-modify-written).
// Errors are the negative neko::proc::Error values (kErrIo for a failed transfer).
#pragma once

#include <neko/types.hpp>

namespace neko::block {

struct Ops {
    // `count` sectors at `lba` to/from a kernel buffer; 0 or an Error. Called from threads only (may block).
    int64_t (*read)(void* context, uint64_t lba, uint32_t count, void* buffer);
    int64_t (*write)(void* context, uint64_t lba, uint32_t count, const void* buffer);
    int64_t (*flush)(void* context);  // may be null (nothing cached)
};

// Field names match sdk/Nekotia.Sdk/IO/Block.cs.
struct Info {
    uint8_t name[16];
    uint8_t model[40];
    uint64_t sectors;
    uint32_t sector_size;
    bool read_only;
    uint64_t read_sectors;   // statistics
    uint64_t written_sectors;
    bool removed;            // unplugged: every access fails, the name may come back with a new device
};

constexpr uint32_t kMaxDevices = 16;

// Registers a device and creates /dev/NAME; returns its index or an Error.
int32_t add(const char* name, const char* model, uint64_t sectors, uint32_t sector_size, bool read_only,
            const Ops* ops, void* context);
// A disk in the kernel heap (zeroed), e.g. for tests and host mode. Index or Error.
int32_t add_ramdisk(const char* name, uint64_t bytes);
// The device is gone (USB stick pulled out): /dev/NAME disappears, reads and writes fail with kErrIo from now on.
// Must not be called while the driver's ops are still running for it (the driver waits for its own I/O first).
void remove(uint32_t index);
// Hot plug: called (from the thread that added or removed the device) after add / remove. One handler.
using Hotplug = void (*)(uint32_t index, bool added);
void set_hotplug(Hotplug handler);

uint32_t count();
bool info(uint32_t index, Info& out);
int32_t find(const char* name);  // -1 if absent
inline int32_t find(const uint8_t* name) { return find(reinterpret_cast<const char*>(name)); }

int64_t read(uint32_t index, uint64_t lba, uint32_t count, void* buffer);
int64_t write(uint32_t index, uint64_t lba, uint32_t count, const void* buffer);
int64_t flush(uint32_t index);
// Byte-addressed access (what /dev/NAME uses): bytes transferred, short at the end of the disk.
int64_t read_bytes(uint32_t index, uint64_t offset, void* buffer, uint64_t length);
int64_t write_bytes(uint32_t index, uint64_t offset, const void* buffer, uint64_t length);

}  // namespace neko::block

namespace neko::drivers {
void init();  // probes PCI for supported devices (virtio-blk, …); x86_64 only
}
