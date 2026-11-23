// NVMe: SSDs on PCIe (QEMU: `-device nvme,serial=…,drive=…`).
//
//   BAR0 registers: CAP, CC (enable, queue entry sizes), CSTS (ready), AQA/ASQ/ACQ (admin queues),
//   doorbells at 0x1000 (stride from CAP.DSTRD)
//   admin queue (polled at start-up): IDENTIFY controller / namespace, create one I/O queue pair
//   I/O queue: 64-byte commands in, 16-byte completions out, a phase bit tells new entries apart
//
// Namespace 1 becomes nvme0n1. Slots as in the other block drivers: each owns a 64 KiB DMA buffer and
// a PRP list for it; the command id is the slot number. Completions by MSI-X (entry 0) or polling.
#include <neko/block.hpp>
#include <neko/process.hpp>
#include <neko/runtime.hpp>

namespace neko::drivers {
namespace {

using namespace proc;  // error codes

constexpr uint32_t kCap = 0x00, kCc = 0x14, kCsts = 0x1C, kAqa = 0x24, kAsq = 0x28, kAcq = 0x30;
constexpr uint8_t kAdminCreateSq = 0x01, kAdminCreateCq = 0x05, kAdminIdentify = 0x06;
constexpr uint8_t kIoFlush = 0x00, kIoWrite = 0x01, kIoRead = 0x02;
constexpr uint16_t kQueue = 64;  // entries per queue (admin and I/O)
constexpr uint32_t kSlots = 8;
constexpr uint32_t kSlotBytes = 64 * 1024;

struct Command {  // submission queue entry
    uint8_t opcode;
    uint8_t flags;
    uint16_t id;
    uint32_t nsid;
    uint64_t reserved;
    uint64_t metadata;
    uint64_t prp1, prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
};
static_assert(sizeof(Command) == 64, "NVMe commands are 64 bytes");

struct Completion {
    uint32_t result;
    uint32_t reserved;
    uint16_t sq_head, sq_id;
    uint16_t id;
    volatile uint16_t status;  // bit 0: phase, bits 1-15: status
};

struct Queue {
    Command* sq;
    Completion* cq;
    uint16_t sq_tail, cq_head;
    uint16_t phase;
    volatile uint32_t* sq_doorbell;
    volatile uint32_t* cq_doorbell;
};

struct Drive {
    volatile uint8_t* regs;
    Queue admin, io;
    uint32_t nsid;
    uint32_t sector;
    uint64_t* prp_lists[kSlots];  // one page each: PRP entries for the slot buffer
    uint8_t* buffers[kSlots];
    sync::Semaphore done[kSlots];
    volatile uint16_t status[kSlots];
    volatile uint32_t outstanding;
    uint32_t free_mask;
    sync::SpinLock lock;
    sync::Semaphore free_slots;
    bool interrupts;
    char name[16];
};

uint32_t r32(volatile uint8_t* b, uint32_t o) { return *reinterpret_cast<volatile uint32_t*>(b + o); }
uint64_t r64(volatile uint8_t* b, uint32_t o) { return uint64_t(r32(b, o)) | (uint64_t(r32(b, o + 4)) << 32); }
void w32(volatile uint8_t* b, uint32_t o, uint32_t v) { *reinterpret_cast<volatile uint32_t*>(b + o) = v; }
void w64(volatile uint8_t* b, uint32_t o, uint64_t v) {
    w32(b, o, uint32_t(v));
    w32(b, o + 4, uint32_t(v >> 32));
}

uint32_t g_drives = 0;

bool make_queue(Queue& q, volatile uint8_t* regs, uint32_t stride, uint16_t id) {
    q.sq = static_cast<Command*>(pmm::alloc_page());       // 64 × 64 B
    q.cq = static_cast<Completion*>(pmm::alloc_page());    // 64 × 16 B
    if (!q.sq || !q.cq) return false;
    q.sq_tail = q.cq_head = 0;
    q.phase = 1;
    q.sq_doorbell = reinterpret_cast<volatile uint32_t*>(regs + 0x1000 + (2 * id) * stride);
    q.cq_doorbell = reinterpret_cast<volatile uint32_t*>(regs + 0x1000 + (2 * id + 1) * stride);
    return true;
}

void submit(Queue& q, const Command& c) {
    q.sq[q.sq_tail] = c;
    q.sq_tail = uint16_t((q.sq_tail + 1) % kQueue);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    *q.sq_doorbell = q.sq_tail;
}

// Next completion if the controller has posted one (its phase bit matches ours).
bool next(Queue& q, Completion& out) {
    Completion& e = q.cq[q.cq_head];
    if ((e.status & 1) != q.phase) return false;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    out = e;
    if (++q.cq_head == kQueue) {
        q.cq_head = 0;
        q.phase ^= 1;
    }
    *q.cq_doorbell = q.cq_head;
    return true;
}

// Admin command, polled (start-up only). Status 0 = success.
uint16_t admin(Drive* d, Command c, uint64_t timeout_ms = 2000) {
    c.id = 0xFFFF;
    submit(d->admin, c);
    uint64_t until = time::uptime_ms() + timeout_ms;
    Completion e;
    while (!next(d->admin, e)) {
        if (time::uptime_ms() > until) return 0xFFFF;
        cpu::pause();
    }
    return uint16_t(e.status >> 1);
}

void reap(Drive* d) {
    sync::ScopedLock guard(d->lock);
    Completion e;
    while (next(d->io, e)) {
        uint16_t s = e.id;
        if (s >= kSlots || !(d->outstanding & (1u << s))) continue;
        d->status[s] = uint16_t(e.status >> 1);
        d->outstanding &= ~(1u << s);
        d->done[s].signal();
    }
}

void on_interrupt(void* context) { reap(static_cast<Drive*>(context)); }

uint32_t take_slot(Drive* d) {
    d->free_slots.wait();
    sync::ScopedLock guard(d->lock);
    uint32_t s = uint32_t(__builtin_ctz(d->free_mask));
    d->free_mask &= ~(1u << s);
    return s;
}

void give_slot(Drive* d, uint32_t s) {
    {
        sync::ScopedLock guard(d->lock);
        d->free_mask |= 1u << s;
    }
    d->free_slots.signal();
}

// I/O command on slot `s` covering `bytes` of its buffer (PRP1 + PRP2/list).
int64_t io(Drive* d, uint32_t s, uint8_t opcode, uint64_t lba, uint32_t count, uint32_t bytes) {
    Command c{};
    c.opcode = opcode;
    c.id = uint16_t(s);
    c.nsid = d->nsid;
    if (bytes) {
        c.prp1 = pmm::virt_to_phys(d->buffers[s]);
        uint32_t pages = (bytes + 4095) / 4096;
        if (pages == 2) c.prp2 = c.prp1 + 4096;
        else if (pages > 2) c.prp2 = pmm::virt_to_phys(d->prp_lists[s]);  // entries for pages 2…
    }
    c.cdw10 = uint32_t(lba);
    c.cdw11 = uint32_t(lba >> 32);
    c.cdw12 = count ? count - 1 : 0;  // 0-based block count
    {
        sync::ScopedLock guard(d->lock);
        d->outstanding |= 1u << s;
        submit(d->io, c);
    }
    if (d->interrupts) {
        d->done[s].wait();
    } else {
        for (;;) {
            reap(d);
            if (!(d->outstanding & (1u << s))) break;
            sched::yield();
        }
        d->done[s].try_wait();
    }
    return d->status[s] == 0 ? 0 : kErrIo;
}

int64_t transfer(Drive* d, uint64_t lba, uint32_t count, uint8_t* buffer, bool write) {
    uint32_t s = take_slot(d);
    uint32_t per = kSlotBytes / d->sector;
    int64_t r = 0;
    while (count && r == 0) {
        uint32_t n = count < per ? count : per;
        uint32_t bytes = n * d->sector;
        if (write) mem::copy(d->buffers[s], buffer, bytes);
        r = io(d, s, write ? kIoWrite : kIoRead, lba, n, bytes);
        if (r == 0 && !write) mem::copy(buffer, d->buffers[s], bytes);
        buffer += bytes;
        lba += n;
        count -= n;
    }
    give_slot(d, s);
    return r;
}

int64_t drive_read(void* context, uint64_t lba, uint32_t count, void* buffer) {
    return transfer(static_cast<Drive*>(context), lba, count, static_cast<uint8_t*>(buffer), false);
}

int64_t drive_write(void* context, uint64_t lba, uint32_t count, const void* buffer) {
    return transfer(static_cast<Drive*>(context), lba, count, static_cast<uint8_t*>(const_cast<void*>(buffer)), true);
}

int64_t drive_flush(void* context) {
    auto* d = static_cast<Drive*>(context);
    uint32_t s = take_slot(d);
    int64_t r = io(d, s, kIoFlush, 0, 0, 0);
    give_slot(d, s);
    return r;
}

const block::Ops kOps = {drive_read, drive_write, drive_flush};

void fail(const char* why) {
    term::write("[neko] nvme: \x1b[93m");
    term::write(why);
    term::write("\x1b[0m — controller ignored\n");
}

}  // namespace

void probe_nvme(uint32_t pci) {
    auto* regs = static_cast<volatile uint8_t*>(pci::map_bar(pci, 0));
    if (!regs) return fail("no register BAR");
    auto* d = static_cast<Drive*>(heap::alloc_zeroed(sizeof(Drive)));
    if (!d) return;
    d->regs = regs;
    pci::enable(pci, true, false, true);

    uint64_t cap = r64(regs, kCap);
    uint32_t stride = 4u << ((cap >> 32) & 0xF);
    uint64_t timeout = ((cap >> 24) & 0xFF) * 500 + 500;  // CAP.TO is in 500 ms units
    if (((cap >> 37) & 1) == 0) return fail("NVM command set not supported");
    if ((cap & 0xFFFF) + 1 < kQueue) return fail("queues too small");

    // Disable, set up the admin queues, enable.
    w32(regs, kCc, 0);
    uint64_t until = time::uptime_ms() + timeout;
    while (r32(regs, kCsts) & 1)
        if (time::uptime_ms() > until) return fail("does not stop");
    if (!make_queue(d->admin, regs, stride, 0)) return fail("out of memory");
    w32(regs, kAqa, uint32_t(kQueue - 1) | (uint32_t(kQueue - 1) << 16));
    w64(regs, kAsq, pmm::virt_to_phys(d->admin.sq));
    w64(regs, kAcq, pmm::virt_to_phys(d->admin.cq));
    w32(regs, kCc, 1 | (6u << 16) | (4u << 20));  // enable, 4 KiB pages, 64-byte SQ / 16-byte CQ entries
    until = time::uptime_ms() + timeout;
    while (!(r32(regs, kCsts) & 1)) {
        if (r32(regs, kCsts) & 2) return fail("fatal status while starting");
        if (time::uptime_ms() > until) return fail("does not become ready");
    }

    auto* identify = static_cast<uint8_t*>(pmm::alloc_page());
    if (!identify) return fail("out of memory");
    Command c{};
    c.opcode = kAdminIdentify;
    c.prp1 = pmm::virt_to_phys(identify);
    c.cdw10 = 1;  // controller
    if (admin(d, c) != 0) return fail("IDENTIFY controller failed");
    char model[41];
    uint32_t n = 0;
    for (uint32_t i = 24; i < 64 && n < 40; ++i) model[n++] = char(identify[i]);
    while (n > 0 && model[n - 1] == ' ') --n;
    model[n] = 0;
    uint32_t namespaces = identify[516] | (identify[517] << 8) | (identify[518] << 16) | (uint32_t(identify[519]) << 24);
    if (namespaces == 0) return fail("no namespaces");

    d->nsid = 1;
    mem::set(identify, 0, 4096);
    c = Command{};
    c.opcode = kAdminIdentify;
    c.nsid = 1;
    c.prp1 = pmm::virt_to_phys(identify);
    c.cdw10 = 0;  // namespace
    if (admin(d, c) != 0) return fail("IDENTIFY namespace failed");
    uint64_t sectors;
    mem::copy(&sectors, identify, 8);
    uint8_t format = identify[26] & 0xF;
    uint32_t lbaf;
    mem::copy(&lbaf, identify + 128 + 4 * format, 4);
    d->sector = 1u << ((lbaf >> 16) & 0xFF);
    pmm::free_page(identify);
    if (d->sector < 512 || d->sector > 4096) return fail("unsupported sector size");

    // I/O queue pair 1, completion interrupt on MSI-X entry 0.
    uint8_t vector = 0;
    if (pci::msix_enable(pci) > 0) {
        vector = irq::allocate_msi(&on_interrupt, d, "nvme");
        if (vector && !pci::msix_route(pci, 0, vector)) vector = 0;
    }
    if (!make_queue(d->io, regs, stride, 1)) return fail("out of memory");
    c = Command{};
    c.opcode = kAdminCreateCq;
    c.prp1 = pmm::virt_to_phys(d->io.cq);
    c.cdw10 = 1 | (uint32_t(kQueue - 1) << 16);
    c.cdw11 = 1 | (vector ? 2u : 0u);  // physically contiguous, interrupts on (vector 0 of MSI-X)
    if (admin(d, c) != 0) return fail("cannot create the completion queue");
    c = Command{};
    c.opcode = kAdminCreateSq;
    c.prp1 = pmm::virt_to_phys(d->io.sq);
    c.cdw10 = 1 | (uint32_t(kQueue - 1) << 16);
    c.cdw11 = 1 | (1u << 16);  // physically contiguous, completions to queue 1
    if (admin(d, c) != 0) return fail("cannot create the submission queue");
    d->interrupts = vector != 0;

    for (uint32_t s = 0; s < kSlots; ++s) {
        d->buffers[s] = static_cast<uint8_t*>(pmm::alloc_contiguous(kSlotBytes / 4096));
        d->prp_lists[s] = static_cast<uint64_t*>(pmm::alloc_page());
        if (!d->buffers[s] || !d->prp_lists[s]) return fail("out of memory");
        uint64_t base = pmm::virt_to_phys(d->buffers[s]);
        for (uint32_t i = 1; i < kSlotBytes / 4096; ++i) d->prp_lists[s][i - 1] = base + uint64_t(i) * 4096;
        d->free_mask |= 1u << s;
    }
    d->free_slots.count = kSlots;

    char name[16] = "nvme0n1";
    name[4] = char('0' + g_drives++);
    str::copy(d->name, name, sizeof(d->name));
    if (block::add(d->name, model, sectors, d->sector, false, &kOps, d) < 0) return fail("block device table is full");
    term::write("[neko] nvme: /dev/");
    term::write(d->name);
    term::write(", ");
    term::write(model);
    term::write(", ");
    term::write_size(sectors * d->sector);
    term::write(", ");
    term::write_udec(d->sector);
    term::write("-byte sectors");
    term::write(d->interrupts ? ", MSI-X\n" : ", polling\n");
}

}  // namespace neko::drivers
