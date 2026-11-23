// AHCI (Serial ATA): the controller of real PCs and of QEMU's q35 machine (ICH9, 00:1f.2), where
// `-device ide-hd,bus=ide.N` attaches disks and the boot CD-ROM sits on port 2.
//
//   ABAR (BAR5) → global registers, then one register block per port (0x100 + 0x80 × port)
//   per port: a command list (32 headers), a received-FIS area, and per command slot a command
//   table (the command FIS + a scatter list, here a single entry pointing at the slot's DMA buffer)
//
// ATA disks become sda, sdb… (READ/WRITE DMA EXT, FLUSH CACHE EXT, 48-bit LBA); ATAPI drives
// become cd0… read-only with 2048-byte sectors (SCSI READ(10) through the PACKET command).
// Slots work like the virtio-blk driver: each has its own 64 KiB buffer, completions arrive by
// MSI (the handler compares the port's command-issue bits with what is outstanding), or are polled.
#include <neko/block.hpp>
#include <neko/process.hpp>
#include <neko/runtime.hpp>

namespace neko::drivers {
namespace {

using namespace proc;  // error codes

// Global registers
constexpr uint32_t kCap = 0x00, kGhc = 0x04, kIs = 0x08, kPi = 0x0C;
constexpr uint32_t kGhcAe = 1u << 31, kGhcIe = 1u << 1;
// Port registers
constexpr uint32_t kPxClb = 0x00, kPxFb = 0x08, kPxIs = 0x10, kPxIe = 0x14, kPxCmd = 0x18, kPxTfd = 0x20,
                   kPxSig = 0x24, kPxSsts = 0x28, kPxSerr = 0x30, kPxCi = 0x38;
constexpr uint32_t kCmdSt = 1, kCmdFre = 1u << 4, kCmdFr = 1u << 14, kCmdCr = 1u << 15;
constexpr uint32_t kIsTfes = 1u << 30;        // task file error
constexpr uint32_t kSigAta = 0x00000101, kSigAtapi = 0xEB140101;

constexpr uint8_t kAtaIdentify = 0xEC, kAtaIdentifyPacket = 0xA1, kAtaReadDmaExt = 0x25, kAtaWriteDmaExt = 0x35,
                  kAtaFlushExt = 0xEA, kAtaPacket = 0xA0;

constexpr uint32_t kSlots = 8;
constexpr uint32_t kSlotBytes = 64 * 1024;

struct CommandHeader {
    uint16_t flags;  // bits 0-4: FIS length in dwords, 5: ATAPI, 6: write
    uint16_t prdt_length;
    volatile uint32_t prd_byte_count;
    uint64_t table;  // physical, 128-byte aligned
    uint32_t reserved[4];
};

struct CommandTable {
    uint8_t fis[64];
    uint8_t atapi[16];
    uint8_t reserved[48];
    struct {
        uint64_t address;
        uint32_t reserved;
        uint32_t count;  // bytes - 1; bit 31: interrupt on completion
    } prdt[1];
};

struct Port {
    volatile uint8_t* regs;
    uint32_t number;
    bool atapi;
    uint32_t sector;           // 512 (ATA) or 2048 (ATAPI)
    CommandHeader* headers;    // command list (1 KiB)
    CommandTable* tables[kSlots];
    uint8_t* buffers[kSlots];
    sync::Semaphore done[kSlots];
    volatile uint32_t outstanding;  // slots issued and not yet completed
    volatile uint32_t failed;       // slots that completed with an error
    uint32_t free_mask;
    uint32_t slots;
    sync::SpinLock lock;
    sync::Semaphore free_slots;
    bool interrupts;
    char name[8];
};

struct Controller {
    volatile uint8_t* abar;
    Port* ports[32];
};

uint32_t r32(volatile uint8_t* b, uint32_t o) { return *reinterpret_cast<volatile uint32_t*>(b + o); }
void w32(volatile uint8_t* b, uint32_t o, uint32_t v) { *reinterpret_cast<volatile uint32_t*>(b + o) = v; }

uint32_t g_disks = 0, g_cds = 0;

// Completes every issued slot whose command-issue bit the controller has cleared.
void reap(Port* p) {
    sync::ScopedLock guard(p->lock);
    uint32_t is = r32(p->regs, kPxIs);
    w32(p->regs, kPxIs, is);  // acknowledge
    uint32_t ci = r32(p->regs, kPxCi);
    uint32_t finished = p->outstanding & ~ci;
    if (is & kIsTfes) {  // a failed command stops the port: fail everything issued
        finished = p->outstanding;
        p->failed |= finished;
    }
    p->outstanding &= ~finished;
    for (uint32_t s = 0; s < p->slots; ++s)
        if (finished & (1u << s)) p->done[s].signal();
}

void on_interrupt(void* context) {
    auto* c = static_cast<Controller*>(context);
    uint32_t is = r32(c->abar, kIs);
    for (uint32_t i = 0; i < 32; ++i)
        if ((is & (1u << i)) && c->ports[i]) reap(c->ports[i]);
    w32(c->abar, kIs, is);
}

uint32_t take_slot(Port* p) {
    p->free_slots.wait();
    sync::ScopedLock guard(p->lock);
    uint32_t s = uint32_t(__builtin_ctz(p->free_mask));
    p->free_mask &= ~(1u << s);
    return s;
}

void give_slot(Port* p, uint32_t s) {
    {
        sync::ScopedLock guard(p->lock);
        p->free_mask |= 1u << s;
    }
    p->free_slots.signal();
}

// Restarts a port after an error (clears the error state; the command is reported failed).
void recover(Port* p) {
    w32(p->regs, kPxCmd, r32(p->regs, kPxCmd) & ~kCmdSt);
    for (int i = 0; i < 1000000 && (r32(p->regs, kPxCmd) & kCmdCr); ++i) cpu::pause();
    w32(p->regs, kPxSerr, 0xFFFFFFFF);
    w32(p->regs, kPxIs, 0xFFFFFFFF);
    w32(p->regs, kPxCmd, r32(p->regs, kPxCmd) | kCmdSt);
}

// Issues the command prepared in slot `s` (FIS, maybe ATAPI packet) moving `bytes` of its buffer.
int64_t issue(Port* p, uint32_t s, uint8_t command, uint64_t lba, uint16_t count, uint32_t bytes, bool write,
              const uint8_t* packet) {
    CommandTable* t = p->tables[s];
    mem::set(t, 0, sizeof(CommandTable));
    uint8_t* f = t->fis;
    f[0] = 0x27;  // register FIS, host to device
    f[1] = 0x80;  // command (not control)
    f[2] = command;
    f[4] = uint8_t(lba);
    f[5] = uint8_t(lba >> 8);
    f[6] = uint8_t(lba >> 16);
    f[7] = 0x40;  // LBA mode
    f[8] = uint8_t(lba >> 24);
    f[9] = uint8_t(lba >> 32);
    f[10] = uint8_t(lba >> 40);
    f[12] = uint8_t(count);
    f[13] = uint8_t(count >> 8);
    if (packet) {
        f[3] = 1;               // features: DMA
        f[5] = uint8_t(bytes);  // byte count limit for PIO fallbacks
        f[6] = uint8_t(bytes >> 8);
        mem::copy(t->atapi, packet, 12);
    }
    CommandHeader& h = p->headers[s];
    h.flags = uint16_t(5 | (packet ? 1u << 5 : 0) | (write ? 1u << 6 : 0));
    h.prdt_length = bytes ? 1 : 0;
    h.prd_byte_count = 0;
    if (bytes) {
        t->prdt[0].address = pmm::virt_to_phys(p->buffers[s]);
        t->prdt[0].count = (bytes - 1) | (1u << 31);
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    {
        sync::ScopedLock guard(p->lock);
        p->failed &= ~(1u << s);
        p->outstanding |= 1u << s;
        w32(p->regs, kPxCi, 1u << s);
    }
    if (p->interrupts) {
        p->done[s].wait();
    } else {
        for (;;) {
            reap(p);
            if (!(p->outstanding & (1u << s))) break;
            sched::yield();
        }
        p->done[s].try_wait();
    }
    if (p->failed & (1u << s)) {
        recover(p);
        return kErrIo;
    }
    return 0;
}

int64_t transfer(Port* p, uint64_t lba, uint32_t count, uint8_t* buffer, bool write) {
    uint32_t s = take_slot(p);
    uint32_t per = kSlotBytes / p->sector;
    int64_t r = 0;
    while (count && r == 0) {
        uint32_t n = count < per ? count : per;
        uint32_t bytes = n * p->sector;
        if (write) mem::copy(p->buffers[s], buffer, bytes);
        if (p->atapi) {
            uint8_t packet[12] = {0x28, 0, uint8_t(lba >> 24), uint8_t(lba >> 16), uint8_t(lba >> 8), uint8_t(lba), 0,
                                  uint8_t(n >> 8), uint8_t(n), 0, 0, 0};  // READ(10)
            r = issue(p, s, kAtaPacket, 0, 0, bytes, false, packet);
        } else {
            r = issue(p, s, write ? kAtaWriteDmaExt : kAtaReadDmaExt, lba, uint16_t(n), bytes, write, nullptr);
        }
        if (r == 0 && !write) mem::copy(buffer, p->buffers[s], bytes);
        buffer += bytes;
        lba += n;
        count -= n;
    }
    give_slot(p, s);
    return r;
}

int64_t port_read(void* context, uint64_t lba, uint32_t count, void* buffer) {
    return transfer(static_cast<Port*>(context), lba, count, static_cast<uint8_t*>(buffer), false);
}

int64_t port_write(void* context, uint64_t lba, uint32_t count, const void* buffer) {
    return transfer(static_cast<Port*>(context), lba, count, static_cast<uint8_t*>(const_cast<void*>(buffer)), true);
}

int64_t port_flush(void* context) {
    auto* p = static_cast<Port*>(context);
    uint32_t s = take_slot(p);
    int64_t r = issue(p, s, kAtaFlushExt, 0, 0, 0, false, nullptr);
    give_slot(p, s);
    return r;
}

const block::Ops kDiskOps = {port_read, port_write, port_flush};
const block::Ops kCdOps = {port_read, nullptr, nullptr};

void stop(volatile uint8_t* regs) {
    w32(regs, kPxCmd, r32(regs, kPxCmd) & ~(kCmdSt | kCmdFre));
    for (int i = 0; i < 1000000 && (r32(regs, kPxCmd) & (kCmdCr | kCmdFr)); ++i) cpu::pause();
}

// "ATA strings": pairs of characters swapped, padded with spaces.
void ata_string(const uint16_t* words, uint32_t count, char* out, uint32_t capacity) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < count && n + 2 < capacity; ++i) {
        out[n++] = char(words[i] >> 8);
        out[n++] = char(words[i]);
    }
    while (n > 0 && out[n - 1] == ' ') --n;
    out[n] = 0;
}

void setup_port(Controller* c, uint32_t number, uint32_t command_slots, uint8_t vector) {
    volatile uint8_t* regs = c->abar + 0x100 + 0x80 * number;
    uint32_t ssts = r32(regs, kPxSsts);
    if ((ssts & 0xF) != 3 || ((ssts >> 8) & 0xF) != 1) return;  // no device, or not active
    uint32_t sig = r32(regs, kPxSig);
    if (sig != kSigAta && sig != kSigAtapi) return;  // port multipliers, enclosure bridges…

    auto* p = static_cast<Port*>(heap::alloc_zeroed(sizeof(Port)));
    if (!p) return;
    p->regs = regs;
    p->number = number;
    p->atapi = sig == kSigAtapi;
    p->sector = p->atapi ? 2048 : 512;
    p->slots = command_slots < kSlots ? command_slots : kSlots;

    stop(regs);
    // Command list (1 KiB) + received FIS (256 B) + one 256-byte table per slot: all in one page.
    auto* page = static_cast<uint8_t*>(pmm::alloc_page());
    if (!page) return;
    p->headers = reinterpret_cast<CommandHeader*>(page);
    uint8_t* fis = page + 1024;
    for (uint32_t s = 0; s < p->slots; ++s) {
        p->tables[s] = reinterpret_cast<CommandTable*>(page + 1280 + 256 * s);
        p->headers[s].table = pmm::virt_to_phys(p->tables[s]);
        p->buffers[s] = static_cast<uint8_t*>(pmm::alloc_contiguous(kSlotBytes / 4096));
        if (!p->buffers[s]) return;
        p->free_mask |= 1u << s;
    }
    p->free_slots.count = p->slots;
    uint64_t clb = pmm::virt_to_phys(page), fb = pmm::virt_to_phys(fis);
    w32(regs, kPxClb, uint32_t(clb));
    w32(regs, kPxClb + 4, uint32_t(clb >> 32));
    w32(regs, kPxFb, uint32_t(fb));
    w32(regs, kPxFb + 4, uint32_t(fb >> 32));
    w32(regs, kPxSerr, 0xFFFFFFFF);
    w32(regs, kPxIs, 0xFFFFFFFF);
    w32(regs, kPxCmd, r32(regs, kPxCmd) | kCmdFre);
    w32(regs, kPxCmd, r32(regs, kPxCmd) | kCmdSt);
    p->interrupts = vector != 0;
    w32(regs, kPxIe, p->interrupts ? 0x7D80007F : 0);  // completions and every error
    c->ports[number] = p;

    // IDENTIFY: size and model.
    uint32_t s = take_slot(p);
    int64_t r = issue(p, s, p->atapi ? kAtaIdentifyPacket : kAtaIdentify, 0, 0, 512, false, nullptr);
    auto* id = reinterpret_cast<uint16_t*>(p->buffers[s]);
    char model[41];
    ata_string(id + 27, 20, model, sizeof(model));
    uint64_t sectors = 0;
    if (r == 0 && !p->atapi) {
        bool lba48 = id[83] & (1u << 10);
        sectors = lba48 ? (uint64_t(id[100]) | (uint64_t(id[101]) << 16) | (uint64_t(id[102]) << 32) | (uint64_t(id[103]) << 48))
                        : (uint64_t(id[60]) | (uint64_t(id[61]) << 16));
    } else if (r == 0) {  // READ CAPACITY(10): last LBA and block size
        uint8_t packet[12] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        if (issue(p, s, kAtaPacket, 0, 0, 8, false, packet) == 0) {
            uint8_t* b = p->buffers[s];
            sectors = ((uint64_t(b[0]) << 24) | (b[1] << 16) | (b[2] << 8) | b[3]) + 1;
        }
    }
    give_slot(p, s);
    if (r < 0) {
        term::write("[neko] ahci: port ");
        term::write_udec(number);
        term::write(": IDENTIFY failed — ignored\n");
        c->ports[number] = nullptr;
        stop(regs);
        return;
    }

    if (p->atapi) {
        p->name[0] = 'c';
        p->name[1] = 'd';
        p->name[2] = char('0' + g_cds++);
        p->name[3] = 0;
    } else {
        p->name[0] = 's';
        p->name[1] = 'd';
        p->name[2] = char('a' + g_disks++);
        p->name[3] = 0;
    }
    if (sectors) block::add(p->name, model, sectors, p->sector, p->atapi, p->atapi ? &kCdOps : &kDiskOps, p);
    term::write("[neko] ahci: port ");
    term::write_udec(number);
    term::write(" /dev/");
    term::write(p->name);
    term::write(", ");
    term::write(p->atapi ? "ATAPI " : "ATA ");
    term::write(model);
    term::write(", ");
    term::write_size(sectors * p->sector);
    if (!sectors) term::write(" (no medium)");
    term::write(p->interrupts ? ", MSI\n" : ", polling\n");
}

}  // namespace

void probe_ahci(uint32_t pci) {
    auto* abar = static_cast<volatile uint8_t*>(pci::map_bar(pci, 5));
    if (!abar) return;
    auto* c = static_cast<Controller*>(heap::alloc_zeroed(sizeof(Controller)));
    if (!c) return;
    c->abar = abar;
    pci::enable(pci, true, false, true);
    w32(abar, kGhc, r32(abar, kGhc) | kGhcAe);
    uint32_t cap = r32(abar, kCap);
    uint32_t command_slots = ((cap >> 8) & 0x1F) + 1;
    if (!(cap & (1u << 31))) {  // 32-bit DMA only: our buffers may live above 4 GiB
        term::write("[neko] ahci: controller without 64-bit DMA — ignored\n");
        return;
    }
    uint8_t vector = irq::allocate_msi(&on_interrupt, c, "ahci");
    if (vector && !pci::msi_enable(pci, vector)) vector = 0;
    w32(abar, kIs, 0xFFFFFFFF);
    if (vector) w32(abar, kGhc, r32(abar, kGhc) | kGhcIe);
    uint32_t implemented = r32(abar, kPi);
    for (uint32_t i = 0; i < 32; ++i)
        if (implemented & (1u << i)) setup_port(c, i, command_slots, vector);
}

}  // namespace neko::drivers
