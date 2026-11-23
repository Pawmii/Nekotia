// Fatal errors: the Nekotia "stop screen".
//
// Every way the kernel can die ends here — unhandled C# exceptions (`throw`), failed
// assertions, Debug.Panic, heap corruption, out of memory and CPU exceptions — and
// produces one screen with a stop code, the C# location, a symbolized call stack,
// the CPU state and the system state. Text is mirrored to the serial port.
#pragma once

#include <neko/types.hpp>

namespace neko::crash {

enum class StopCode : uint32_t {
    // 0x00–0x1F: CPU exception vectors (#DE = 0x00, #UD = 0x06, #DF = 0x08, #GP = 0x0D, #PF = 0x0E …)
    UnhandledException = 0xE0,  // C# `throw` that nothing catches (try/catch is not supported yet)
    AssertionFailed = 0xE1,
    KernelPanic = 0xE2,
    HeapCorruption = 0xE3,
    OutOfMemory = 0xE4,
    Deadlock = 0xE5,            // mutex locked twice by its owner, or a cycle of threads waiting for each other
};

// Register state captured by the interrupt stubs.
struct CpuState {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

// C# `throw new T(message)` (lowered by nekoc).
NEKO_NORETURN void unhandled_exception(const char* type, const char* message, const char* file, int32_t line,
                                       const char* function);
// Debug.Assert / Debug.Panic with caller information.
NEKO_NORETURN void assert_failed(const char* message, const char* file, int32_t line, const char* function);
NEKO_NORETURN void panic_at(const char* message, const char* file, int32_t line, const char* function);
// Runtime failures.
NEKO_NORETURN void fail(StopCode code, const char* message);
// CPU exception from the IDT.
NEKO_NORETURN void cpu_exception(const CpuState* state);

bool in_progress();  // a stop screen is being drawn (other CPUs halt on the NMI it sends)

// Symbolization (uses the table embedded at link time; empty in the first link pass and in host mode).
struct Symbol {
    const char* function;  // nullptr when unknown
    uint64_t offset;       // address - function start
    const char* file;      // nullptr when unknown
    uint32_t line;
};
Symbol symbolize(uint64_t address);

}  // namespace neko::crash
