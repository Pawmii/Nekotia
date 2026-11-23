// virtio-blk: the disk QEMU attaches with `-device virtio-blk-pci` (transport: virtio.cpp).
//
// One request queue. Requests use fixed 3-descriptor chains (header, data, status), one per slot;
// each slot owns a 64 KiB DMA buffer, so callers' buffers never need to be physically contiguous.
// Completions arrive by MSI-X (the used ring is reaped in the interrupt handler, which wakes the
// waiting thread); without MSI-X the waiting thread polls the used ring and yields.
#include <neko/block.hpp>
#include <neko/process.hpp>

#include "virtio.hpp"

namespace neko::drivers {
namespace {

using namespace proc;  // error codes
using virtio::Desc;

constexpr uint32_t kFeatureBlkRo = 5, kFeatureBlkFlush = 9;
constexpr uint32_t kReqIn = 0, kReqOut = 1, kReqFlush = 4;

struct RequestHeader {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

constexpr uint32_t kSlots = 8;
constexpr uint32_t kSlotBytes = 64 * 1024;
constexpr uint32_t kSlotPages = kSlotBytes / 4096;
constexpr uint32_t kSector = 512;

struct Slot {
    RequestHeader* header;     // DMA
    volatile uint8_t* status;  // DMA
    uint8_t* data;             // DMA, kSlotBytes
    sync::Semaphore done;
    volatile bool complete;
};

struct Disk {
    virtio::Device dev;
    virtio::Queue queue;
    Slot slots[kSlots];
    uint32_t slot_count;
    uint32_t free_mask;
    sync::SpinLock slot_lock;
    sync::Semaphore free_slots;
    bool interrupts;  // MSI-X routed; otherwise poll
    bool flush;
    char name[8];
};

// Marks every finished request complete (interrupt handler, or a polling thread).
void reap(Disk* d) {
    sync::ScopedLock guard(d->queue.lock);
    uint32_t head, length;
    while (virtio::next_used(d->queue, head, length)) {
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
    Desc* c = &d->queue.desc[head];
    c[0] = {pmm::virt_to_phys(slot.header), sizeof(RequestHeader), virtio::kDescNext, uint16_t(head + 2)};
    if (bytes) {
        uint16_t flags = uint16_t(virtio::kDescNext | (type == kReqIn ? virtio::kDescWrite : 0));
        c[1] = {pmm::virt_to_phys(slot.data), bytes, flags, uint16_t(head + 2)};
        c[0].next = uint16_t(head + 1);
    }
    c[2] = {pmm::virt_to_phys(const_cast<uint8_t*>(slot.status)), 1, virtio::kDescWrite, 0};
    {
        sync::ScopedLock guard(d->queue.lock);
        virtio::publish(d->queue, head);
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
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
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

uint32_t g_disks = 0;

}  // namespace

void probe_virtio_blk(uint32_t pci) {
    auto* d = static_cast<Disk*>(heap::alloc_zeroed(sizeof(Disk)));
    if (!d) return;
    if (!virtio::attach(d->dev, pci)) return virtio::fail(d->dev, "virtio-blk", "no virtio 1.0 capabilities (legacy-only device)");
    uint64_t features = virtio::negotiate(d->dev, 0, (1ull << kFeatureBlkRo) | (1ull << kFeatureBlkFlush));
    if (!features) return virtio::fail(d->dev, "virtio-blk", "features not accepted");
    d->flush = features & (1ull << kFeatureBlkFlush);

    uint8_t vector = d->dev.msix ? irq::allocate_msi(&on_interrupt, d, "virtio-blk") : 0;
    if (!virtio::setup_queue(d->dev, d->queue, 0, 64, vector, 0)) return virtio::fail(d->dev, "virtio-blk", "no request queue");
    d->interrupts = d->queue.interrupts;

    // Slots: header + status in one page, a 64 KiB buffer each.
    d->slot_count = d->queue.size / 3 < kSlots ? d->queue.size / 3 : kSlots;
    auto* small = static_cast<uint8_t*>(pmm::alloc_page());
    if (!small) return virtio::fail(d->dev, "virtio-blk", "out of memory");
    for (uint32_t s = 0; s < d->slot_count; ++s) {
        d->slots[s].header = reinterpret_cast<RequestHeader*>(small + s * 32);
        d->slots[s].status = small + s * 32 + 16;
        d->slots[s].data = static_cast<uint8_t*>(pmm::alloc_contiguous(kSlotPages));
        if (!d->slots[s].data) return virtio::fail(d->dev, "virtio-blk", "out of memory");
        d->free_mask |= 1u << s;
    }
    d->free_slots.count = d->slot_count;
    virtio::driver_ok(d->dev);

    uint64_t sectors = virtio::config64(d->dev, 0);
    bool read_only = features & (1ull << kFeatureBlkRo);
    d->name[0] = 'v';
    d->name[1] = 'd';
    d->name[2] = char('a' + g_disks++);
    d->name[3] = 0;
    if (block::add(d->name, "virtio-blk", sectors, kSector, read_only, &kOps, d) < 0)
        return virtio::fail(d->dev, "virtio-blk", "block device table is full");
    term::write("[neko] virtio-blk: /dev/");
    term::write(d->name);
    term::write(", ");
    term::write_size(sectors * kSector);
    term::write(read_only ? ", read-only" : "");
    term::write(d->interrupts ? ", MSI-X" : ", polling");
    term::write(", queue ");
    term::write_udec(d->queue.size);
    term::write("\n");
}

}  // namespace neko::drivers
