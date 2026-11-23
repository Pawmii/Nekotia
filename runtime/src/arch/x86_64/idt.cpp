// Interrupt descriptor table with handlers for the 32 CPU exceptions.
// Every exception prints the register state to the serial port and panics,
// so a bug in kernel code shows up as a readable report instead of a triple-fault reboot.
// IRQs (PIC/APIC, timer, keyboard) come in ROADMAP phase 2.
#include <neko/runtime.hpp>

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

// Layout produced by isr.S (lowest address first).
struct InterruptFrame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

alignas(16) IdtEntry g_idt[256];

const char* const kExceptionNames[32] = {
    "divide error", "debug", "NMI", "breakpoint", "overflow", "bound range", "invalid opcode", "device not available",
    "double fault", "coprocessor segment overrun", "invalid TSS", "segment not present", "stack-segment fault",
    "general protection fault", "page fault", "reserved", "x87 floating point", "alignment check", "machine check",
    "SIMD floating point", "virtualization", "control protection", "reserved", "reserved", "reserved", "reserved",
    "reserved", "reserved", "hypervisor injection", "VMM communication", "security", "reserved",
};

void reg(const char* name, uint64_t value) {
    serial::write("  ");
    serial::write(name);
    serial::write("=");
    serial::write_hex(value);
}

}  // namespace

extern "C" void* neko_isr_table[32];

extern "C" void neko_isr_dispatch(InterruptFrame* f) {
    serial::write("\n=== CPU exception #");
    serial::write_udec(f->vector);
    serial::write(" (");
    serial::write(f->vector < 32 ? kExceptionNames[f->vector] : "unknown");
    serial::write(") ===\n");
    reg("rip", f->rip);
    reg("err", f->error_code);
    reg("rflags", f->rflags);
    reg("cs", f->cs);
    serial::write("\n");
    if (f->vector == 14) {
        reg("cr2", cpu::read_cr2());
        serial::write(f->error_code & 1 ? "  (protection violation" : "  (page not present");
        serial::write(f->error_code & 2 ? ", write" : ", read");
        serial::write(f->error_code & 16 ? ", instruction fetch)\n" : ")\n");
    }
    reg("rax", f->rax); reg("rbx", f->rbx); reg("rcx", f->rcx); reg("rdx", f->rdx); serial::write("\n");
    reg("rsi", f->rsi); reg("rdi", f->rdi); reg("rbp", f->rbp); reg("rsp", f->rsp); serial::write("\n");
    reg("r8", f->r8);   reg("r9", f->r9);   reg("r10", f->r10); reg("r11", f->r11); serial::write("\n");
    reg("r12", f->r12); reg("r13", f->r13); reg("r14", f->r14); reg("r15", f->r15); serial::write("\n");
    serial::write("  hint: addr2line -e build/nekotia.elf <rip> maps rip back to the C# line\n");
    panic("unhandled CPU exception");
}

void idt_init() {
    uint16_t cs;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));

    for (int i = 0; i < 32; ++i) {
        auto handler = reinterpret_cast<uint64_t>(neko_isr_table[i]);
        IdtEntry& e = g_idt[i];
        e.offset_low = handler & 0xFFFF;
        e.selector = cs;
        e.ist = 0;
        e.type_attr = 0x8E;  // present, ring 0, 64-bit interrupt gate
        e.offset_mid = (handler >> 16) & 0xFFFF;
        e.offset_high = static_cast<uint32_t>(handler >> 32);
        e.zero = 0;
    }

    IdtPointer ptr = {sizeof(g_idt) - 1, reinterpret_cast<uint64_t>(g_idt)};
    __asm__ volatile("lidt %0" : : "m"(ptr));
}

}  // namespace neko::arch
