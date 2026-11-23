// CPU / port I/O primitives (x86_64). In host builds they are harmless stubs.
#pragma once

#include <neko/types.hpp>

namespace neko::cpu {

#if NEKO_HOSTED

void halt();  // host: ends the process (the kernel is done)
NEKO_NORETURN void halt_forever();
inline void disable_interrupts() {}
inline void enable_interrupts() {}
inline void outb(uint16_t, uint8_t) {}
inline void outw(uint16_t, uint16_t) {}
inline void outl(uint16_t, uint32_t) {}
inline uint8_t inb(uint16_t) { return 0; }
inline uint16_t inw(uint16_t) { return 0; }
inline uint32_t inl(uint16_t) { return 0; }
inline uint64_t read_cr2() { return 0; }
inline uint64_t read_cr3() { return 0; }

#else

inline void halt() { __asm__ volatile("hlt"); }
inline void disable_interrupts() { __asm__ volatile("cli" ::: "memory"); }
inline void enable_interrupts() { __asm__ volatile("sti" ::: "memory"); }

NEKO_NORETURN inline void halt_forever() {
    for (;;) __asm__ volatile("cli; hlt");
}

inline void outb(uint16_t port, uint8_t v) { __asm__ volatile("outb %0, %1" ::"a"(v), "Nd"(port)); }
inline void outw(uint16_t port, uint16_t v) { __asm__ volatile("outw %0, %1" ::"a"(v), "Nd"(port)); }
inline void outl(uint16_t port, uint32_t v) { __asm__ volatile("outl %0, %1" ::"a"(v), "Nd"(port)); }

inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
inline uint16_t inw(uint16_t port) {
    uint16_t v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
inline uint32_t inl(uint16_t port) {
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

inline uint64_t read_cr2() {
    uint64_t v;
    __asm__ volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}
inline uint64_t read_cr3() {
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

#endif

inline void io_wait() { outb(0x80, 0); }

inline uint64_t read_tsc() {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

}  // namespace neko::cpu
