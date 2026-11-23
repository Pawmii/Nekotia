// Interrupt controllers and time:
//   * the legacy 8259 PIC is remapped out of the way and fully masked
//   * the local APIC delivers a 1 kHz periodic timer (calibrated against PIT channel 2)
//   * the IOAPIC routes legacy ISA IRQs (keyboard, …) to vectors 0x30 + irq
#include "arch.hpp"

#include <neko/sched.hpp>

namespace neko::arch {
namespace {

// Local APIC registers
constexpr uint32_t kLapicId = 0x020;
constexpr uint32_t kLapicTpr = 0x080;
constexpr uint32_t kLapicEoi = 0x0B0;
constexpr uint32_t kLapicSvr = 0x0F0;
constexpr uint32_t kLapicLvtTimer = 0x320;
constexpr uint32_t kLapicInitialCount = 0x380;
constexpr uint32_t kLapicCurrentCount = 0x390;
constexpr uint32_t kLapicDivide = 0x3E0;
constexpr uint32_t kMasked = 1u << 16;
constexpr uint32_t kPeriodic = 1u << 17;

constexpr uint32_t kTimerHz = 1000;

volatile uint32_t* g_lapic = nullptr;
uint32_t g_bsp_apic_id = 0;
MadtInfo g_madt;
bool g_apic_ready = false;

volatile uint64_t g_ticks = 0;
uint64_t g_tsc_per_ms = 0;
uint32_t g_cpu_count = 1;

irq::Handler g_handlers[16];
volatile uint64_t g_irq_counts[16];

uint32_t lapic_read(uint32_t reg) { return g_lapic[reg / 4]; }
void lapic_write(uint32_t reg, uint32_t value) { g_lapic[reg / 4] = value; }

void ioapic_write(uint64_t phys, uint32_t reg, uint32_t value) {
    auto* io = static_cast<volatile uint32_t*>(vmm::map_mmio(phys, 0x20));
    io[0] = reg;
    io[4] = value;
}

uint32_t ioapic_read(uint64_t phys, uint32_t reg) {
    auto* io = static_cast<volatile uint32_t*>(vmm::map_mmio(phys, 0x20));
    io[0] = reg;
    return io[4];
}

void pic_disable() {
    using namespace neko::cpu;
    // Remap to 0x20/0x28 first so a stray PIC interrupt can never look like a CPU exception.
    outb(0x20, 0x11); io_wait();
    outb(0xA0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait();
    outb(0xA1, 0x28); io_wait();
    outb(0x21, 0x04); io_wait();
    outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait();
    outb(0xA1, 0x01); io_wait();
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
}

void mask_all_ioapic_lines() {
    for (uint32_t i = 0; i < g_madt.ioapic_count; ++i) {
        uint32_t max = (ioapic_read(g_madt.ioapics[i].phys, 0x01) >> 16) & 0xFF;
        for (uint32_t pin = 0; pin <= max; ++pin) ioapic_write(g_madt.ioapics[i].phys, 0x10 + pin * 2, kMasked);
    }
}

// LAPIC ticks (divider 16) and TSC cycles in 10 ms, measured with PIT channel 2 in one-shot mode.
void calibrate(uint32_t& lapic_per_ms, uint64_t& tsc_per_ms) {
    using namespace neko::cpu;
    constexpr uint16_t kPitCount = 11932;  // 1193182 Hz × 10 ms

    lapic_write(kLapicDivide, 0x3);
    uint8_t port61 = inb(0x61) & 0xFC;   // gate low, speaker off
    outb(0x61, port61);
    outb(0x43, 0xB0);                     // channel 2, lobyte/hibyte, mode 0
    outb(0x42, kPitCount & 0xFF);
    outb(0x42, kPitCount >> 8);

    lapic_write(kLapicInitialCount, 0xFFFFFFFF);
    uint64_t tsc_start = read_tsc();
    outb(0x61, port61 | 0x01);            // gate high: the PIT starts counting
    while ((inb(0x61) & 0x20) == 0) {
    }
    uint64_t tsc_end = read_tsc();
    uint32_t elapsed = 0xFFFFFFFF - lapic_read(kLapicCurrentCount);
    lapic_write(kLapicInitialCount, 0);

    lapic_per_ms = elapsed / 10;
    tsc_per_ms = (tsc_end - tsc_start) / 10;
}

const MadtInfo::IoApic* ioapic_for(uint32_t gsi) {
    for (uint32_t i = 0; i < g_madt.ioapic_count; ++i) {
        const auto& io = g_madt.ioapics[i];
        uint32_t max = (ioapic_read(io.phys, 0x01) >> 16) & 0xFF;
        if (gsi >= io.gsi_base && gsi <= io.gsi_base + max) return &io;
    }
    return nullptr;
}

}  // namespace

void apic_init(const MadtInfo& madt) {
    g_madt = madt;
    g_cpu_count = madt.cpu_count ? madt.cpu_count : 1;

    pic_disable();
    g_lapic = static_cast<volatile uint32_t*>(vmm::map_mmio(madt.lapic_phys, 0x1000));
    g_bsp_apic_id = lapic_read(kLapicId) >> 24;
    lapic_write(kLapicTpr, 0);
    lapic_write(kLapicSvr, 0x100 | kVectorSpurious);  // software enable
    mask_all_ioapic_lines();

    uint32_t lapic_per_ms = 0;
    calibrate(lapic_per_ms, g_tsc_per_ms);
    if (lapic_per_ms == 0) panic("apic: timer calibration failed");

    lapic_write(kLapicLvtTimer, kVectorTimer | kPeriodic);
    lapic_write(kLapicDivide, 0x3);
    lapic_write(kLapicInitialCount, lapic_per_ms * 1000 / kTimerHz);
    g_apic_ready = true;
}

void irq_dispatch(uint8_t vector) {
    if (vector == kVectorSpurious) return;  // no EOI for spurious interrupts
    if (vector == kVectorTimer) {
        g_ticks = g_ticks + 1;
        lapic_write(kLapicEoi, 0);
        sched::tick();
    } else {
        if (vector >= kVectorIrqBase && vector < kVectorIrqBase + 16) {
            uint8_t line = vector - kVectorIrqBase;
            g_irq_counts[line] = g_irq_counts[line] + 1;
            if (g_handlers[line]) g_handlers[line]();
        }
        lapic_write(kLapicEoi, 0);
    }
    // After the EOI, so the next timer interrupt can reach whichever thread runs next.
    sched::preempt_if_needed();
}

}  // namespace neko::arch

// ---------------------------------------------------------------- public services

namespace neko::irq {

bool register_handler(uint8_t line, Handler handler) {
    using namespace neko::arch;
    if (line >= 16 || !g_apic_ready) return false;

    uint32_t gsi = line;
    uint16_t flags = 0;
    for (uint32_t i = 0; i < g_madt.override_count; ++i)
        if (g_madt.overrides[i].source == line) gsi = g_madt.overrides[i].gsi, flags = g_madt.overrides[i].flags;

    const MadtInfo::IoApic* io = ioapic_for(gsi);
    if (!io) return false;

    uint32_t low = kVectorIrqBase + line;
    if ((flags & 0x3) == 0x3) low |= 1u << 13;          // active low
    if (((flags >> 2) & 0x3) == 0x3) low |= 1u << 15;   // level triggered
    uint32_t pin = gsi - io->gsi_base;
    g_handlers[line] = handler;
    ioapic_write(io->phys, 0x10 + pin * 2 + 1, g_bsp_apic_id << 24);
    ioapic_write(io->phys, 0x10 + pin * 2, low);
    return true;
}

uint64_t count(uint8_t line) { return line < 16 ? arch::g_irq_counts[line] : 0; }

}  // namespace neko::irq

namespace neko::time {

uint64_t uptime_ms() { return arch::g_ticks; }
uint32_t timer_hz() { return arch::kTimerHz; }

void sleep_ms(uint64_t ms) {
    if (!arch::g_apic_ready) return;  // no timer: nothing would ever wake us up
    sched::sleep_ms(ms);              // other threads run meanwhile
}

}  // namespace neko::time

namespace neko::sysinfo {

const char* cpu_brand() {
    static char brand[49];
    if (!brand[0]) {
        uint32_t max_ext;
        __asm__ volatile("cpuid" : "=a"(max_ext) : "a"(0x80000000) : "ebx", "ecx", "edx");
        if (max_ext < 0x80000004) return "unknown x86_64 CPU";
        auto* out = reinterpret_cast<uint32_t*>(brand);
        for (uint32_t leaf = 0; leaf < 3; ++leaf)
            __asm__ volatile("cpuid"
                             : "=a"(out[leaf * 4]), "=b"(out[leaf * 4 + 1]), "=c"(out[leaf * 4 + 2]), "=d"(out[leaf * 4 + 3])
                             : "a"(0x80000002 + leaf));
    }
    const char* p = brand;
    while (*p == ' ') ++p;
    return p;
}

uint32_t cpu_count() { return arch::g_cpu_count; }
uint64_t cpu_mhz() { return arch::g_tsc_per_ms / 1000; }
bool apic_available() { return arch::g_apic_ready; }

}  // namespace neko::sysinfo

namespace neko::power {

void reboot() {
    cpu::disable_interrupts();
    // 8042 keyboard controller pulse reset line, then the PCI reset register.
    for (int i = 0; i < 1000 && (cpu::inb(0x64) & 0x02); ++i) {
    }
    cpu::outb(0x64, 0xFE);
    cpu::outb(0xCF9, 0x06);
    // Last resort: triple fault with an empty IDT.
    struct __attribute__((packed)) {
        uint16_t limit;
        uint64_t base;
    } empty = {0, 0};
    __asm__ volatile("lidt %0; int3" : : "m"(empty));
    cpu::halt_forever();
}

void test_exit(uint8_t code) {
    cpu::outb(0xf4, code);
    shutdown();  // no debug-exit device: power off instead
}

void shutdown() {
    cpu::disable_interrupts();
    cpu::outw(0x604, 0x2000);   // QEMU (q35/piix with ACPI)
    cpu::outw(0xB004, 0x2000);  // Bochs, old QEMU
    cpu::outw(0x4004, 0x3400);  // VirtualBox
    term::write("\nIt is now safe to turn off the computer.\n");
    cpu::halt_forever();
}

}  // namespace neko::power
