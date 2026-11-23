// The Nekotia stop screen. See neko/crash.hpp.
//
// Layout (160×50 console at 1280×800):
//    /\_/\    NEKOTIA HAS STOPPED                       ← title drawn 2× large
//   ( x.x )   Котик упал. …
//    > ^ <
//   STOP 0x0000000E  PAGE_FAULT
//   exception / message / where (C# file:line)
//   ── call stack ── (symbolized from the table embedded at link time)
//   ── cpu ──        (only for CPU exceptions)
//   ── system ──
//   press R to reboot
#include <neko/crash.hpp>
#include <neko/runtime.hpp>

// ---------------------------------------------------------------- embedded symbol table (scripts/gen-symbols.py)

struct SymFunc {
    uint64_t address;
    uint32_t size;
    uint32_t name;  // offset into strings
};
struct SymLine {
    uint32_t offset;  // address - kSymbolBase
    uint32_t line;    // 0 = end of sequence
    uint32_t file;    // index into files
};
struct SymTable {
    uint64_t function_count;
    uint64_t line_count;
    uint64_t file_count;
    const SymFunc* functions;
    const SymLine* lines;
    const uint32_t* files;  // offsets into strings
    const char* strings;
};

// Weak: absent in the first link pass and in host builds.
extern "C" __attribute__((weak)) const SymTable neko_symtab;

namespace neko::crash {
namespace {

constexpr uint64_t kSymbolBase = 0xffffffff80000000ull;

// Palette of the stop screen.
constexpr uint32_t kBg = 0x2A0F3D;
constexpr uint32_t kText = 0xF2E7FF;
constexpr uint32_t kMuted = 0xB49CC8;
constexpr uint32_t kAccent = 0xFF7AB6;
constexpr uint32_t kChipFg = 0x2A0F3D;
constexpr uint32_t kGood = 0x8FE3A8;
constexpr uint32_t kWarn = 0xFFD37A;

struct Report {
    StopCode code;
    uint32_t vector;  // CPU exceptions only
    const char* code_name;
    const char* exception;  // C#-style exception type
    const char* message;
    const char* detail;     // extra line ("write to non-present page …")
    uint64_t detail_address;
    const char* file;
    int32_t line;
    const char* function;
    const CpuState* cpu;
    uint64_t first_return;  // software crashes: return address into the failing code
    uint64_t frame;         // rbp to start the stack walk from
};

bool g_crashing = false;

const char* const kVectorNames[32] = {
    "DIVIDE_ERROR", "DEBUG", "NMI", "BREAKPOINT", "OVERFLOW", "BOUND_RANGE_EXCEEDED", "INVALID_OPCODE",
    "DEVICE_NOT_AVAILABLE", "DOUBLE_FAULT", "COPROCESSOR_SEGMENT_OVERRUN", "INVALID_TSS", "SEGMENT_NOT_PRESENT",
    "STACK_SEGMENT_FAULT", "GENERAL_PROTECTION_FAULT", "PAGE_FAULT", "RESERVED_15", "X87_FLOATING_POINT",
    "ALIGNMENT_CHECK", "MACHINE_CHECK", "SIMD_FLOATING_POINT", "VIRTUALIZATION", "CONTROL_PROTECTION",
    "RESERVED_22", "RESERVED_23", "RESERVED_24", "RESERVED_25", "RESERVED_26", "RESERVED_27",
    "HYPERVISOR_INJECTION", "VMM_COMMUNICATION", "SECURITY", "RESERVED_31",
};

// ---------------------------------------------------------------- output helpers

void color(uint32_t fg, uint32_t bg = kBg) { console::set_colors(fg, bg); }
void text(const char* s) { term::write(s); }
void nl() { term::write_char('\n'); }
void hex(uint64_t v) { term::write_hex(v); }
void dec(uint64_t v) { term::write_udec(v); }

void hex16(uint64_t v) {  // fixed width: 0xffffffff80001234
    static constexpr char kDigits[] = "0123456789abcdef";
    text("0x");
    for (int shift = 60; shift >= 0; shift -= 4) term::write_char(uint8_t(kDigits[(v >> shift) & 0xF]));
}

void hex8(uint32_t v) {  // stop codes: 0x0000000E
    static constexpr char kDigits[] = "0123456789ABCDEF";
    text("0x");
    for (int shift = 28; shift >= 0; shift -= 4) term::write_char(uint8_t(kDigits[(v >> shift) & 0xF]));
}

void pad(size_t written, size_t width) {
    for (size_t i = written; i < width; ++i) term::write_char(' ');
}

void section(const char* title) {
    nl();
    color(kAccent);
    text("  ── ");
    text(title);
    text(" ");
    for (size_t i = str::length(title); i < 40; ++i) text("─");
    nl();
    color(kText);
}

void label(const char* name) {
    color(kMuted);
    text("  ");
    text(name);
    pad(str::length(name), 11);
    color(kText);
}

void reg(const char* name, uint64_t v) {
    color(kMuted);
    text("  ");
    pad(str::length(name), 3);
    text(name);
    text(" ");
    color(kText);
    hex16(v);
}

// "Nekotia::OS::Commands::Crash" is printed as-is; the table already stores C#-style names for C# code.
void location(const Symbol& s, uint64_t address) {
    if (s.function) {
        color(kText);
        text(s.function);
        if (s.offset) {
            color(kMuted);
            text("+");
            hex(s.offset);
        }
    } else {
        color(kMuted);
        text("?? ");
        hex(address);
    }
    if (s.file) {
        color(kMuted);
        text("  at ");
        color(kWarn);
        text(s.file);
        text(":");
        dec(s.line);
    }
    color(kText);
}

// ---------------------------------------------------------------- CPU details

uint64_t read_cr(int n) {
#if NEKO_HOSTED
    (void)n;
    return 0;
#else
    uint64_t v = 0;
    switch (n) {
        case 0: __asm__ volatile("mov %%cr0, %0" : "=r"(v)); break;
        case 2: __asm__ volatile("mov %%cr2, %0" : "=r"(v)); break;
        case 3: __asm__ volatile("mov %%cr3, %0" : "=r"(v)); break;
        case 4: __asm__ volatile("mov %%cr4, %0" : "=r"(v)); break;
    }
    return v;
#endif
}

// What a .NET developer would call this CPU exception.
void classify(Report& r) {
    const CpuState& f = *r.cpu;
    switch (f.vector) {
        case 0:
            r.exception = "System.DivideByZeroException";
            r.message = "Attempted to divide by zero (or the quotient overflowed).";
            break;
        case 4:
            r.exception = "System.OverflowException";
            r.message = "Arithmetic overflow (INTO).";
            break;
        case 5:
            r.exception = "System.IndexOutOfRangeException";
            r.message = "BOUND range exceeded.";
            break;
        case 6:
            r.exception = "System.InvalidProgramException";
            r.message = "The CPU met an invalid instruction (ud2, a corrupted return address or a jump into data).";
            break;
        case 8: {
            uint64_t cr2 = read_cr(2);
            bool guard = vmm::is_stack_guard(cr2) || vmm::is_stack_guard(f.rsp - 8);
            r.exception = "System.StackOverflowException";
            r.message = guard ? "The kernel stack overflowed into its guard page (deep or infinite recursion, or a huge stackalloc)."
                              : "Double fault: an exception happened while handling another one — usually a stack overflow.";
            if (guard) {
                r.detail = "guard page hit at";
                r.detail_address = vmm::is_stack_guard(cr2) ? cr2 : f.rsp - 8;
            }
            break;
        }
        case 13:
            r.exception = "System.AccessViolationException";
            r.message = "General protection fault: non-canonical address, bad segment or a privileged instruction.";
            break;
        case 14: {
            uint64_t cr2 = read_cr(2);
            bool present = f.error_code & 1, write = f.error_code & 2, fetch = f.error_code & 16;
            bool null_ref = cr2 < 0x10000;
            r.exception = null_ref ? "System.NullReferenceException" : "System.AccessViolationException";
            r.message = null_ref               ? "Object reference not set to an instance of an object (access near address 0)."
                        : vmm::is_stack_guard(cr2) ? "Access to a kernel stack guard page (stack overflow)."
                        : present && fetch     ? "Attempted to execute memory that is not executable (NX: heap, stack or data)."
                        : present && write     ? "Attempted to write read-only memory (kernel code or constant data)."
                        : present              ? "Access to a page the kernel is not allowed to touch."
                                               : "Attempted to read or write memory that is not mapped.";
            if (vmm::is_stack_guard(cr2)) r.exception = "System.StackOverflowException";
            r.detail = fetch ? (present ? "instruction fetch from a non-executable page at" : "instruction fetch from an unmapped page at")
                       : write ? (present ? "write to a protected page at" : "write to an unmapped page at")
                               : (present ? "read from a protected page at" : "read from an unmapped page at");
            r.detail_address = cr2;
            break;
        }
        case 3:
            r.exception = "System.Diagnostics.Debugger.Break";
            r.message = "Breakpoint (int3) without a debugger attached.";
            break;
        default:
            r.exception = "CPU exception";
            r.message = "The processor raised an exception the kernel does not handle.";
    }
}

// ---------------------------------------------------------------- stack walk

bool plausible(uint64_t rbp) {
    if (rbp == 0 || (rbp & 7) != 0) return false;
#if !NEKO_HOSTED
    if (rbp < 0xffff800000000000ull) return false;  // kernel stacks live in the higher half
#endif
    return true;
}

void frame_line(int index, uint64_t address, bool is_return) {
    Symbol s = symbolize(is_return ? address - 1 : address);
    color(kMuted);
    text("  #");
    dec(index);
    text(index < 10 ? "  " : " ");
    hex16(address);
    text("  ");
    location(s, address);
    nl();
}

void stack(const Report& r) {
    section("call stack");
    int index = 0;
    uint64_t rbp = r.frame;
    if (r.cpu) {
        frame_line(index++, r.cpu->rip, false);
        // rip outside any known function (a jump into data, a bad function pointer): the caller's
        // return address is still on top of the stack.
        if (&neko_symtab && !symbolize(r.cpu->rip).function && plausible(r.cpu->rsp))
            frame_line(index++, *reinterpret_cast<uint64_t*>(r.cpu->rsp), true);
    } else if (r.first_return) {
        frame_line(index++, r.first_return, true);
        rbp = plausible(rbp) ? *reinterpret_cast<uint64_t*>(rbp) : 0;  // skip the crash entry point's own frame
    }
    uint64_t last = 0, repeats = 0;
    for (uint64_t walked = 0; index < 14 && plausible(rbp) && walked < 1000000; ++walked) {
        auto* frame = reinterpret_cast<uint64_t*>(rbp);
        uint64_t ret = frame[1];
        if (ret == 0) break;
        if (ret == last) {
            ++repeats;  // recursion: fold identical frames into one line
        } else {
            if (repeats) {
                color(kMuted);
                text("      … the frame above repeats ");
                dec(repeats);
                text(" more times\n");
                repeats = 0;
            }
            frame_line(index++, ret, true);
            last = ret;
        }
        uint64_t next = frame[0];
        if (next <= rbp) break;  // the stack grows down; callers are at higher addresses
        rbp = next;
    }
    if (repeats) {
        color(kMuted);
        text("      … the frame above repeats ");
        dec(repeats);
        text(" more times\n");
    }
    if (!&neko_symtab) {
        color(kMuted);
        text("  (no symbol table in this build — use: addr2line -e build/nekotia.elf <address>)\n");
    }
}

// ---------------------------------------------------------------- screen

void title(const Report& r) {
    bool screen = console::ready();
    color(kText);
    console::clear();
    console::set_cursor_visible(false);
    text("\n");
    color(kAccent);
    text("     /\\_/\\\n");
    text("    ( x.x )\n");
    text("     > ^ <\n");
    if (screen) {
        console::draw_large(16, 1, "NEKOTIA HAS STOPPED", kText, kBg, 2);
    } else {
        text("NEKOTIA HAS STOPPED\n");
    }
    color(kMuted);
    text("\n    Котик упал. The kernel hit an error it could not handle and stopped to keep your data safe.\n\n");

    // stop code chip
    text("    ");
    color(kChipFg, kAccent);
    text(" STOP ");
    hex8(uint32_t(r.code));
    text(" ");
    color(kText);
    text("  ");
    color(kAccent);
    text(r.code_name);
    nl();
}

void details(const Report& r) {
    nl();
    if (r.exception) {
        label("exception");
        color(kWarn);
        text(r.exception);
        nl();
    }
    if (r.message) {
        label("message");
        text(r.message);
        nl();
    }
    if (r.detail) {
        label("detail");
        text(r.detail);
        text(" ");
        hex(r.detail_address);
        nl();
    }
    label("where");
    if (r.file) {
        if (r.function) {
            text(r.function);
            color(kMuted);
            text("  at ");
        }
        color(kWarn);
        text(r.file);
        text(":");
        dec(uint64_t(r.line));
        color(kText);
    } else {
        uint64_t at = r.cpu ? r.cpu->rip : r.first_return;
        location(symbolize(r.cpu ? at : at - 1), at);
    }
    nl();
}

void registers(const CpuState& f) {
    section("cpu state");
    reg("rip", f.rip); reg("rsp", f.rsp); reg("rbp", f.rbp); reg("rflags", f.rflags); nl();
    reg("rax", f.rax); reg("rbx", f.rbx); reg("rcx", f.rcx); reg("rdx", f.rdx); nl();
    reg("rsi", f.rsi); reg("rdi", f.rdi); reg("r8", f.r8); reg("r9", f.r9); nl();
    reg("r10", f.r10); reg("r11", f.r11); reg("r12", f.r12); reg("r13", f.r13); nl();
    reg("r14", f.r14); reg("r15", f.r15); reg("cr0", read_cr(0)); reg("cr2", read_cr(2)); nl();
    reg("cr3", read_cr(3)); reg("cr4", read_cr(4)); reg("err", f.error_code); reg("cs", f.cs); nl();
}

void system_state() {
    section("system");
    uint64_t ms = time::uptime_ms();
    label("uptime");
    dec(ms / 1000);
    text(".");
    uint64_t frac = ms % 1000;
    if (frac < 100) text("0");
    if (frac < 10) text("0");
    dec(frac);
    text(" s");
    color(kMuted);
    text("    cpu ");
    color(kText);
    text(sysinfo::cpu_brand());
    nl();

    label("heap");
    term::write_size(heap::used_bytes());
    text(" used of ");
    term::write_size(heap::total_bytes());
    text(", ");
    dec(heap::block_count());
    text(" blocks, check ");
    if (heap::check()) {
        color(kGood);
        text("ok");
    } else {
        color(kAccent);
        text("CORRUPTED");
    }
    color(kMuted);
    text("    pages ");
    color(kText);
    dec(pmm::free_pages());
    text(" free");
    nl();
    label("build");
    text("runtime " __DATE__ " " __TIME__ ", clang " __clang_version__);
    nl();
}

NEKO_NORETURN void show(Report& r) {
    cpu::disable_interrupts();
    if (g_crashing) {  // crashed while drawing the crash screen: keep it minimal
        serial::write("\n*** NEKOTIA: nested crash, halting ***\n");
        platform::stop_after_panic();
    }
    g_crashing = true;

    if (r.cpu) classify(r);
    title(r);
    details(r);
    stack(r);
    if (r.cpu) registers(*r.cpu);
    system_state();

    nl();
    color(kMuted);
    text("  Press ");
    color(kAccent);
    text("R");
    color(kMuted);
    text(" to reboot.  Report the STOP code and the call stack; addr2line -e build/nekotia.elf <address> gives the exact line.\n");
    serial::write("\n");

#if !NEKO_HOSTED
    // Interrupts are off: poll the PS/2 controller for the R key (make code 0x13).
    for (;;) {
        if (cpu::inb(0x64) & 1) {
            if (cpu::inb(0x60) == 0x13) power::reboot();
        }
        cpu::io_wait();
    }
#else
    platform::stop_after_panic();
#endif
}

Report software(StopCode code, const char* name, uint64_t ret, uint64_t frame) {
    Report r{};
    r.code = code;
    r.code_name = name;
    r.first_return = ret;
    r.frame = frame;
    return r;
}

}  // namespace

#define NEKO_CALLER uint64_t(__builtin_return_address(0)), uint64_t(__builtin_frame_address(0))

void unhandled_exception(const char* type, const char* message, const char* file, int32_t line, const char* function) {
    Report r = software(StopCode::UnhandledException, "UNHANDLED_EXCEPTION", NEKO_CALLER);
    r.exception = type;
    r.message = message ? message : "Exception of this type was thrown.";
    r.file = file;
    r.line = line;
    r.function = function;
    show(r);
}

void assert_failed(const char* message, const char* file, int32_t line, const char* function) {
    Report r = software(StopCode::AssertionFailed, "ASSERTION_FAILED", NEKO_CALLER);
    r.exception = "Debug.Assert";
    r.message = message;
    r.file = file;
    r.line = line;
    r.function = function;
    show(r);
}

void panic_at(const char* message, const char* file, int32_t line, const char* function) {
    Report r = software(StopCode::KernelPanic, "KERNEL_PANIC", NEKO_CALLER);
    r.message = message;
    r.file = file;
    r.line = line;
    r.function = function;
    show(r);
}

void fail(StopCode code, const char* message) {
    const char* name = code == StopCode::HeapCorruption ? "HEAP_CORRUPTION"
                       : code == StopCode::OutOfMemory  ? "OUT_OF_MEMORY"
                                                        : "KERNEL_PANIC";
    Report r = software(code, name, NEKO_CALLER);
    r.exception = code == StopCode::OutOfMemory ? "System.OutOfMemoryException" : nullptr;
    r.message = message;
    show(r);
}

void cpu_exception(const CpuState* state) {
    Report r{};
    r.code = StopCode(state->vector);
    r.vector = uint32_t(state->vector);
    r.code_name = state->vector < 32 ? kVectorNames[state->vector] : "UNKNOWN_VECTOR";
    r.cpu = state;
    r.frame = state->rbp;
    show(r);
}

Symbol symbolize(uint64_t address) {
    Symbol s{nullptr, 0, nullptr, 0};
    if (!&neko_symtab) return s;
    const SymTable& t = neko_symtab;

    // Last function starting at or before the address.
    uint64_t lo = 0, hi = t.function_count;
    while (lo < hi) {
        uint64_t mid = (lo + hi) / 2;
        if (t.functions[mid].address <= address) lo = mid + 1;
        else hi = mid;
    }
    if (lo > 0) {
        const SymFunc& f = t.functions[lo - 1];
        if (address < f.address + (f.size ? f.size : 1)) {
            s.function = t.strings + f.name;
            s.offset = address - f.address;
        }
    }

    // Last line-table row at or before the address.
    if (address >= kSymbolBase) {
        uint64_t offset = address - kSymbolBase;
        lo = 0, hi = t.line_count;
        while (lo < hi) {
            uint64_t mid = (lo + hi) / 2;
            if (t.lines[mid].offset <= offset) lo = mid + 1;
            else hi = mid;
        }
        if (lo > 0 && t.lines[lo - 1].line != 0) {
            s.file = t.strings + t.files[t.lines[lo - 1].file];
            s.line = t.lines[lo - 1].line;
        }
    }
    return s;
}

}  // namespace neko::crash
