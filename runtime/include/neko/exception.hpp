// try/catch/finally for C# code (common/exceptions.cpp, common/jump.S), shared by the kernel and
// user programs. nekoc lowers
//
//   try { A } catch (InvalidOperationException e) when (F) { B } finally { C }
//
// to (roughly)
//
//   { auto fin = DeferTag{} + [&]() { C };              // finally = a scope guard
//     ::neko::exc::Frame t;                              // pushes itself; popped on any exit
//     if (neko_setjmp(t.jmp) == 0) { A }
//     else if (is(t.thrown, "System.InvalidOperationException") && F) { B }
//     else rethrow(t.thrown); }
//
// raise() (what `throw` becomes) runs every cleanup registered after the innermost frame, pops
// it and long-jumps into it. No frame, a barrier frame (a C# function C++ may call: its C++
// callers hold locks a jump would skip) or an interrupt handler: unhandled, as before (the
// stop screen in the kernel, exit code 134 in a program).
#pragma once

#include <neko/types.hpp>

extern "C" __attribute__((returns_twice)) int neko_setjmp(uint64_t* buffer);
extern "C" [[noreturn]] void neko_longjmp(uint64_t* buffer, int value);

namespace neko::exc {

struct Thrown {
    const char* chain;     // "Derived\0Base\0System.Exception\0" (double NUL at the end)
    const char* message;
    const char* file;
    int32_t line;
    const char* function;
};

struct Frame {
    Frame* prev;
    Cleanup* cleanups;  // the cleanup list when the frame was entered
    uint64_t jmp[10];
    Thrown thrown;
    bool active;
    bool barrier;

    explicit Frame(bool is_barrier = false) {
        State* s = state();
        prev = s->frames;
        cleanups = s->cleanups;
        active = true;
        barrier = is_barrier;
        s->frames = this;
    }
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
    ~Frame() {
        if (active) state()->frames = prev;
    }
};

NEKO_NORETURN void raise(const char* chain, const char* message, const char* file, int32_t line, const char* function);
NEKO_NORETURN void rethrow(const Thrown& t);
bool is(const Thrown& t, const char* type);  // `type` or one of its bases

}  // namespace neko::exc
