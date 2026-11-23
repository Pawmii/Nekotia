// Interrupt descriptor table: CPU exceptions (0–31) go to the stop screen
// (core/crash.cpp) — or end the process when a user program caused them (core/process.cpp);
// vectors 32–255 go to the IRQ dispatcher (apic.cpp).
#include "arch.hpp"

#include <neko/process.hpp>

namespace neko::arch {
namespace {

struct __attribute__((packed)) IdtEntry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
};

struct __attribute__((packed)) IdtPointer {
    uint16_t limit;
    uint64_t base;
};

alignas(16) IdtEntry g_idt[256];

uint8_t ist_for(int vector) {
    switch (vector) {
        case 8: return 1;   // double fault: always on a known-good stack
        case 2: return 2;   // NMI
        case 18: return 3;  // machine check
        default: return 0;
    }
}

}  // namespace

extern "C" void* neko_isr_table[256];

// isr.S pushes exactly the layout of crash::CpuState.
extern "C" void neko_isr_dispatch(crash::CpuState* f) {
    if (f->vector == 2 && crash::in_progress()) cpu::halt_forever();  // another CPU is drawing the stop screen
    // A fault in a user program ends that process, not the kernel (NMI, #DF and #MC are machine events).
    if (f->vector < 32 && (f->cs & 3) && f->vector != 2 && f->vector != 8 && f->vector != 18) proc::user_exception(f);
    if (f->vector < 32) crash::cpu_exception(f);
    irq_dispatch(static_cast<uint8_t>(f->vector));
}

void idt_init() {
    for (int i = 0; i < 256; ++i) {
        auto handler = reinterpret_cast<uint64_t>(neko_isr_table[i]);
        IdtEntry& e = g_idt[i];
        e.offset_low = handler & 0xFFFF;
        e.selector = kKernelCode;
        e.ist = ist_for(i);
        e.type_attr = 0x8E;  // present, ring 0, 64-bit interrupt gate (IF cleared on entry)
        e.offset_mid = (handler >> 16) & 0xFFFF;
        e.offset_high = static_cast<uint32_t>(handler >> 32);
        e.zero = 0;
    }

    idt_load();
}

void idt_load() {
    IdtPointer ptr = {sizeof(g_idt) - 1, reinterpret_cast<uint64_t>(g_idt)};
    __asm__ volatile("lidt %0" : : "m"(ptr));
}

}  // namespace neko::arch
