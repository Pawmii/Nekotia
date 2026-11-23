// virtio-blk (virtio 1.x over PCI, "modern" transport): the disk QEMU attaches with
// `-drive if=virtio` or `-device virtio-blk-pci`.
//
//   PCI vendor capabilities → common / notify / device configuration windows in a BAR
//   reset → ACKNOWLEDGE → DRIVER → features (VERSION_1, RO, FLUSH) → FEATURES_OK → queue 0 → DRIVER_OK
//
// One split virtqueue. Requests use fixed 3-descriptor chains (header, data, status), one per slot;
// each slot owns a 64 KiB DMA buffer, so callers' buffers never need to be physically contiguous.
// Completions arrive by MSI-X (the used ring is reaped in the interrupt handler, which wakes the
// waiting thread); without MSI-X the waiting thread polls the used ring and yields.
#include <neko/block.hpp>
#include <neko/process.hpp>
#include <neko/runtime.hpp>

namespace neko::drivers {
namespace {

using namespace proc;  // error codes

// ---- virtio constants
constexpr uint8_t kStatusAcknowledge = 1, kStatusDriver = 2, kStatusDriverOk = 4, kStatusFeaturesOk = 8, kStatusFailed = 128;
constexpr uint32_t kFeatureBlkRo = 5, kFeatureBlkFlush = 9, kFeatureVersion1 = 32;
constexpr uint8_t kCapCommon = 1, kCapNotify = 2, kCapIsr = 3, kCapDevice = 4;
constexpr uint16_t kNoVector = 0xFFFF;
constexpr uint32_t kReqIn = 0, kReqOut = 1, kReqFlush = 4;
constexpr uint16_t kDescNext = 1, kDescWrite = 2;

// Common configuration window (virtio 1.x, 4.1.4.3).
enum Common : uint32_t {
    kDeviceFeatureSelect = 0, kDeviceFeature = 4, kDriverFeatureSelect = 8, kDriverFeature = 12,
    kMsixConfig = 16, kNumQueues = 18, kDeviceStatus = 20, kConfigGeneration = 21,
    kQueueSelect = 22, kQueueSize = 24, kQueueMsixVector = 26, kQueueEnable = 28, kQueueNotifyOff = 30,
    kQueueDesc = 32, kQueueDriver = 40, kQueueDevice = 48,
};

struct Desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct RequestHeader {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

constexpr uint32_t kSlots = 8;
constexpr uint32_t kSlotBytes = 64 * 1024;
constexpr uint32_t kSlotPages = kSlotBytes / 4096;
constexpr uint16_t kQueueMax = 64;
constexpr uint32_t kSector = 512;

struct Slot {
    RequestHeader* header;     // DMA
    volatile uint8_t* status;  // DMA
    uint8_t* data;             // DMA, kSlotBytes
    sync::Semaphore done;
    volatile bool complete;
};

struct Disk {
    uint32_t pci;
    volatile uint8_t* common;
    volatile uint8_t* notify_base;
    uint32_t notify_multiplier;
    volatile uint8_t* device;
    volatile uint16_t* notify;  // queue 0's doorbell

    uint16_t size;
    Desc* desc;
    volatile uint16_t* avail;  // flags, idx, ring[size]
    volatile uint8_t* used;    // flags, idx, ring[size] {id, len}
    uint16_t last_used;
    uint16_t avail_idx;
    sync::SpinLock queue_lock;

    Slot slots[kSlots];
    uint32_t slot_count;
    uint32_t free_mask;
    sync::SpinLock slot_lock;
    sync::Semaphore free_slots;
    bool interrupts;  // MSI-X routed; otherwise poll
    bool flush;
    char name[8];
};

// ---- MMIO helpers (little endian, uncached)
uint8_t r8(volatile uint8_t* base, uint32_t off) { return base[off]; }
uint16_t r16(volatile uint8_t* base, uint32_t off) { return *reinterpret_cast<volatile uint16_t*>(base + off); }
uint32_t r32(volatile uint8_t* base, uint32_t off) { return *reinterpret_cast<volatile uint32_t*>(base + off); }
void w8(volatile uint8_t* base, uint32_t off, uint8_t v) { base[off] = v; }
void w16(volatile uint8_t* base, uint32_t off, uint16_t v) { *reinterpret_cast<volatile uint16_t*>(base + off) = v; }
void w32(volatile uint8_t* base, uint32_t off, uint32_t v) { *reinterpret_cast<volatile uint32_t*>(base + off) = v; }
void w64(volatile uint8_t* base, uint32_t off, uint64_t v) {
    w32(base, off, uint32_t(v));
    w32(base, off + 4, uint32_t(v >> 32));
}

void barrier() { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

uint16_t used_idx(Disk* d) { return *reinterpret_cast<volatile uint16_t*>(d->used + 2); }
uint32_t used_id(Disk* d, uint16_t i) { return *reinterpret_cast<volatile uint32_t*>(d->used + 4 + (i % d->size) * 8); }

// Marks every finished request complete (interrupt handler, or a polling thread).
void reap(Disk* d) {
    sync::ScopedLock guard(d->queue_lock);
    while (d->last_used != used_idx(d)) {
        barrier();
        uint32_t head = used_id(d, d->last_used);
        ++d->last_used;
        uint32_t s = head / 3;
        if (s < d->slot_count) {
            d->slots[s].complete = true;
            d->slots[s].done.signal();
        }
    }
}

void on_interrupt(void* context) { reap(static_cast<Disk*>(context)); }

uint32_t take_slot(Disk* d) {
    d->free_slots.wait();
    sync::ScopedLock guard(d->slot_lock);
    uint32_t s = uint32_t(__builtin_ctz(d->free_mask));
    d->free_mask &= ~(1u << s);
    return s;
}

void give_slot(Disk* d, uint32_t s) {
    {
        sync::ScopedLock guard(d->slot_lock);
        d->free_mask |= 1u << s;
    }
    d->free_slots.signal();
}

// One request through slot `s`: `bytes` of the slot's buffer go out (writes) or come in (reads).
int64_t submit(Disk* d, uint32_t s, uint32_t type, uint64_t sector, uint32_t bytes) {
    Slot& slot = d->slots[s];
    slot.header->type = type;
    slot.header->reserved = 0;
    slot.header->sector = sector;
    *slot.status = 0xFF;
    slot.complete = false;

    uint16_t head = uint16_t(s * 3);
    Desc* c = &d->desc[head];
    c[0] = {pmm::virt_to_phys(slot.header), sizeof(RequestHeader), kDescNext, uint16_t(head + 1)};
    if (bytes) {
        c[1] = {pmm::virt_to_phys(slot.data), bytes, uint16_t(kDescNext | (type == kReqIn ? kDescWrite : 0)), uint16_t(head + 2)};
        c[0].next = uint16_t(head + 1);
    } else {
        c[0].next = uint16_t(head + 2);
    }
    c[2] = {pmm::virt_to_phys(const_cast<uint8_t*>(slot.status)), 1, kDescWrite, 0};

    {
        sync::ScopedLock guard(d->queue_lock);
        d->avail[2 + d->avail_idx % d->size] = head;
        barrier();
        ++d->avail_idx;
        d->avail[1] = d->avail_idx;
        barrier();
        *d->notify = 0;  // queue index 0
    }

    if (d->interrupts) {
        slot.done.wait();
    } else {
        while (!slot.complete) {
            reap(d);
            if (!slot.complete) sched::yield();
        }
        slot.done.try_wait();  // reap signalled it: keep the count at zero
    }
    barrier();
    uint8_t status = *slot.status;
    return status == 0 ? 0 : status == 2 ? kErrNotSupported : kErrIo;
}

int64_t disk_read(void* context, uint64_t lba, uint32_t count, void* buffer) {
    auto* d = static_cast<Disk*>(context);
    auto* out = static_cast<uint8_t*>(buffer);
    uint32_t s = take_slot(d);
    int64_t r = 0;
    while (count && r == 0) {
        uint32_t n = count < kSlotBytes / kSector ? count : kSlotBytes / kSector;
        r = submit(d, s, kReqIn, lba, n * kSector);
        if (r == 0) mem::copy(out, d->slots[s].data, uint64_t(n) * kSector);
        out += uint64_t(n) * kSector;
        lba += n;
        count -= n;
    }
    give_slot(d, s);
    return r;
}

int64_t disk_write(void* context, uint64_t lba, uint32_t count, const void* buffer) {
    auto* d = static_cast<Disk*>(context);
    auto* in = static_cast<const uint8_t*>(buffer);
    uint32_t s = take_slot(d);
    int64_t r = 0;
    while (count && r == 0) {
        uint32_t n = count < kSlotBytes / kSector ? count : kSlotBytes / kSector;
        mem::copy(d->slots[s].data, in, uint64_t(n) * kSector);
        r = submit(d, s, kReqOut, lba, n * kSector);
        in += uint64_t(n) * kSector;
        lba += n;
        count -= n;
    }
    give_slot(d, s);
    return r;
}

int64_t disk_flush(void* context) {
    auto* d = static_cast<Disk*>(context);
    if (!d->flush) return 0;
    uint32_t s = take_slot(d);
    int64_t r = submit(d, s, kReqFlush, 0, 0);
    give_slot(d, s);
    return r;
}

const block::Ops kOps = {disk_read, disk_write, disk_flush};

void fail(Disk* d, const char* why) {
    if (d->common) w8(d->common, kDeviceStatus, kStatusFailed);
    term::write("[neko] virtio-blk: \x1b[93m");
    term::write(why);
    term::write("\x1b[0m — disk ignored\n");
}

uint32_t g_disks = 0;

void probe(uint32_t pci) {
    auto* d = static_cast<Disk*>(heap::alloc_zeroed(sizeof(Disk)));
    if (!d) return;
    d->pci = pci;
    pci::enable(pci, true, false, true);

    // Configuration windows from the vendor capabilities.
    for (uint8_t cap = pci::find_capability(pci, 0x09); cap; cap = pci::find_capability(pci, 0x09, cap)) {
        uint8_t type = pci::read8(pci, uint16_t(cap + 3));
        uint8_t bar = pci::read8(pci, uint16_t(cap + 4));
        uint32_t offset = pci::read32(pci, uint16_t(cap + 8));
        auto* base = static_cast<volatile uint8_t*>(pci::map_bar(pci, bar));
        if (!base) continue;
        if (type == kCapCommon && !d->common) d->common = base + offset;
        else if (type == kCapNotify && !d->notify_base) {
            d->notify_base = base + offset;
            d->notify_multiplier = pci::read32(pci, uint16_t(cap + 16));
        } else if (type == kCapDevice && !d->device) d->device = base + offset;
    }
    if (!d->common || !d->notify_base || !d->device) {
        fail(d, "no virtio 1.0 configuration capabilities (legacy-only device)");
        return;
    }

    volatile uint8_t* c = d->common;
    w8(c, kDeviceStatus, 0);  // reset
    for (int i = 0; i < 1000000 && r8(c, kDeviceStatus) != 0; ++i) cpu::pause();
    w8(c, kDeviceStatus, kStatusAcknowledge);
    w8(c, kDeviceStatus, kStatusAcknowledge | kStatusDriver);

    w32(c, kDeviceFeatureSelect, 0);
    uint64_t offered = r32(c, kDeviceFeature);
    w32(c, kDeviceFeatureSelect, 1);
    offered |= uint64_t(r32(c, kDeviceFeature)) << 32;
    if (!(offered & (1ull << kFeatureVersion1))) {
        fail(d, "device does not offer VIRTIO_F_VERSION_1");
        return;
    }
    uint64_t wanted = (1ull << kFeatureVersion1) | (offered & ((1ull << kFeatureBlkRo) | (1ull << kFeatureBlkFlush)));
    w32(c, kDriverFeatureSelect, 0);
    w32(c, kDriverFeature, uint32_t(wanted));
    w32(c, kDriverFeatureSelect, 1);
    w32(c, kDriverFeature, uint32_t(wanted >> 32));
    w8(c, kDeviceStatus, kStatusAcknowledge | kStatusDriver | kStatusFeaturesOk);
    if (!(r8(c, kDeviceStatus) & kStatusFeaturesOk)) {
        fail(d, "features not accepted");
        return;
    }
    d->flush = wanted & (1ull << kFeatureBlkFlush);

    // Queue 0: descriptors, avail ring and used ring in one zeroed page.
    w16(c, kQueueSelect, 0);
    uint16_t max = r16(c, kQueueSize);
    if (max < 3) {
        fail(d, "request queue too small");
        return;
    }
    uint16_t size = kQueueMax;
    while (size > max) size /= 2;
    auto* ring = static_cast<uint8_t*>(pmm::alloc_page());
    if (!ring) {
        fail(d, "out of memory");
        return;
    }
    d->size = size;
    d->desc = reinterpret_cast<Desc*>(ring);
    d->avail = reinterpret_cast<volatile uint16_t*>(ring + 16 * size);
    d->used = ring + 2048;
    w16(c, kQueueSize, size);
    w64(c, kQueueDesc, pmm::virt_to_phys(ring));
    w64(c, kQueueDriver, pmm::virt_to_phys(ring + 16 * size));
    w64(c, kQueueDevice, pmm::virt_to_phys(ring + 2048));

    // Slots: header + status in one page, a 64 KiB buffer each.
    d->slot_count = size / 3 < kSlots ? size / 3 : kSlots;
    auto* small = static_cast<uint8_t*>(pmm::alloc_page());
    if (!small) {
        fail(d, "out of memory");
        return;
    }
    for (uint32_t s = 0; s < d->slot_count; ++s) {
        d->slots[s].header = reinterpret_cast<RequestHeader*>(small + s * 32);
        d->slots[s].status = small + s * 32 + 16;
        d->slots[s].data = static_cast<uint8_t*>(pmm::alloc_contiguous(kSlotPages));
        if (!d->slots[s].data) {
            fail(d, "out of memory");
            return;
        }
        d->free_mask |= 1u << s;
    }
    d->free_slots.count = d->slot_count;

    // Completion interrupt: MSI-X entry 0 → a fresh vector; the configuration-change interrupt stays off.
    w16(c, kMsixConfig, kNoVector);
    if (pci::msix_enable(pci) > 0) {
        uint8_t vector = irq::allocate_msi(&on_interrupt, d, "virtio-blk");
        if (vector && pci::msix_route(pci, 0, vector)) {
            w16(c, kQueueMsixVector, 0);
            d->interrupts = r16(c, kQueueMsixVector) == 0;  // the device says 0xFFFF when it cannot
        }
    }
    if (!d->interrupts) w16(c, kQueueMsixVector, kNoVector);

    uint16_t notify_off = r16(c, kQueueNotifyOff);
    d->notify = reinterpret_cast<volatile uint16_t*>(d->notify_base + uint32_t(notify_off) * d->notify_multiplier);
    w16(c, kQueueEnable, 1);
    w8(c, kDeviceStatus, kStatusAcknowledge | kStatusDriver | kStatusFeaturesOk | kStatusDriverOk);

    uint64_t sectors = uint64_t(r32(d->device, 0)) | (uint64_t(r32(d->device, 4)) << 32);
    bool read_only = wanted & (1ull << kFeatureBlkRo);
    d->name[0] = 'v';
    d->name[1] = 'd';
    d->name[2] = char('a' + g_disks++);
    d->name[3] = 0;
    int32_t index = block::add(d->name, "virtio-blk", sectors, kSector, read_only, &kOps, d);
    if (index < 0) {
        fail(d, "block device table is full");
        return;
    }
    term::write("[neko] virtio-blk: /dev/");
    term::write(d->name);
    term::write(", ");
    term::write_size(sectors * kSector);
    term::write(read_only ? ", read-only" : "");
    term::write(d->interrupts ? ", MSI-X" : ", polling");
    term::write(", queue ");
    term::write_udec(size);
    term::write("\n");
}

}  // namespace

void init() {
    for (int32_t i = pci::find(0x1AF4, 0xFFFF); i >= 0; i = pci::find(0x1AF4, 0xFFFF, uint32_t(i) + 1)) {
        const pci::Device* dev = pci::get(uint32_t(i));
        // 0x1001 = transitional block device, 0x1042 = modern (0x1040 + device type 2).
        if (dev->device_id == 0x1001 || dev->device_id == 0x1042) probe(uint32_t(i));
    }
}

}  // namespace neko::drivers
