// Basic types and language support shared by the runtime and generated code.
#pragma once

#include <stddef.h>
#include <stdint.h>

#if NEKO_HOSTED
#include <new>
#else
inline void* operator new(size_t, void* where) noexcept { return where; }
inline void* operator new[](size_t, void* where) noexcept { return where; }
#endif

namespace neko {

// C# `delegate* unmanaged<A, B, R>` → neko::fnptr<R, A, B>
template <class R, class... Args>
using fnptr = R (*)(Args...);

// C# exceptions (common/exceptions.cpp): every scope guard registers itself on the calling thread's
// cleanup list, so `throw` can run the defers (and finally blocks, and lock releases) between it and
// the catching `try` before it jumps there.
namespace exc {
struct Cleanup {
    Cleanup* prev;
    void (*run)(Cleanup*);
};
struct Frame;
struct State {
    Frame* frames;       // innermost try (or barrier) first
    Cleanup* cleanups;   // innermost scope guard first
    uint32_t irq_depth;  // > 0 inside an interrupt handler: throwing there is always fatal
};
State* state();  // the calling thread's (kernel: its sched::Thread; user programs: TLS)
}  // namespace exc

namespace detail {

// `defer stmt;` (.neko sugar), `lock`, `finally` → auto guard = DeferTag{} + [&]() { stmt; };
// `link` must stay the first member (a cleanup is found from it).
template <class F>
struct Defer {
    exc::Cleanup link;
    F fn;
    bool done;
    explicit Defer(F f) : fn(f), done(false) {
        exc::State* s = exc::state();
        link.prev = s->cleanups;
        link.run = &invoke;
        s->cleanups = &link;
    }
    Defer(const Defer&) = delete;
    Defer& operator=(const Defer&) = delete;
    ~Defer() {
        if (done) return;
        exc::state()->cleanups = link.prev;
        done = true;
        fn();
    }
    static void invoke(exc::Cleanup* c) {  // run by a throw passing through (it already unlinked us)
        auto* d = reinterpret_cast<Defer*>(c);
        d->done = true;
        d->fn();
    }
};

struct DeferTag {};

template <class F>
Defer<F> operator+(DeferTag, F fn) { return Defer<F>(fn); }

}  // namespace detail
}  // namespace neko

// C# allows bitwise operators on enums; C++ `enum class` needs them spelled out.
#define NEKO_ENUM_OPERATORS(E)                                                                       \
    constexpr E operator|(E a, E b) { return (E)((__underlying_type(E))a | (__underlying_type(E))b); } \
    constexpr E operator&(E a, E b) { return (E)((__underlying_type(E))a & (__underlying_type(E))b); } \
    constexpr E operator^(E a, E b) { return (E)((__underlying_type(E))a ^ (__underlying_type(E))b); } \
    constexpr E operator~(E a) { return (E)(~(__underlying_type(E))a); }                              \
    constexpr E& operator|=(E& a, E b) { return a = a | b; }                                           \
    constexpr E& operator&=(E& a, E b) { return a = a & b; }                                           \
    constexpr E& operator^=(E& a, E b) { return a = a ^ b; }

// Inline assembly from C# (Asm.Emit / `asm { }` sugar). Compiled out in host builds.
#if NEKO_HOSTED
#define NEKO_ASM(code) ((void)0)
#define NEKO_ASM_INTEL(code) ((void)0)
#else
#define NEKO_ASM(code) __asm__ volatile(code ::: "memory")
#define NEKO_ASM_INTEL(code) __asm__ volatile(".intel_syntax noprefix\n\t" code "\n\t.att_syntax prefix" ::: "memory")
#endif

#define NEKO_NORETURN [[noreturn]]
#define NEKO_UNUSED [[maybe_unused]]
