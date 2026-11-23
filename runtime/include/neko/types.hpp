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

namespace detail {

// `defer stmt;` (.neko sugar) → auto guard = DeferTag{} + [&]() { stmt; };
template <class F>
struct Defer {
    F fn;
    ~Defer() { fn(); }
};

struct DeferTag {};

template <class F>
Defer<F> operator+(DeferTag, F fn) { return Defer<F>{fn}; }

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
