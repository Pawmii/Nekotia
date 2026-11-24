// Per-CPU data and inter-processor interrupts.
//
// Every CPU's GS base points at its Cpu record (gs:0 is the record itself), so this_cpu()
// is one instruction. CPU 0 is the bootstrap processor; the others are started through
// the Limine MP request (arch/x86_64/smp.cpp).
#pragma once

#include <neko/types.hpp>

namespace neko::sched {
struct Thread;
}

namespace neko::smp {

constexpr uint32_t kMaxCpus = 16;

struct Cpu {
    Cpu* self;            // gs:0  — must stay first: this_cpu() reads it
    uint64_t kernel_rsp;  // gs:8  — top of the current thread's kernel stack (syscall entry, TSS.rsp0)
    uint64_t user_rsp;    // gs:16 — scratch: user rsp while the syscall entry switches stacks
    uint32_t index;
    uint32_t lapic_id;
    volatile bool online;
    sched::Thread* current;
    sched::Thread* idle;
    sched::Thread* zombie;  // exited thread whose stack is released after the next switch
    bool need_resched;
    uint64_t slice_left;
    uint64_t idle_ms;
    uint64_t stack_top;  // kernel stack the CPU runs its boot flow / idle thread on
    volatile uint64_t tlb_seen;  // last TLB shootdown request this CPU has carried out
};

extern Cpu g_cpus[kMaxCpus];
extern bool g_percpu_ready;

inline Cpu* this_cpu() {
#if NEKO_HOSTED
    return &g_cpus[0];
#else
    if (!g_percpu_ready) return &g_cpus[0];
    Cpu* c;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(c));
    return c;
#endif
}

inline uint32_t cpu_index() { return this_cpu()->index; }

uint32_t online_count();
uint32_t cpu_count();  // CPUs reported by the bootloader (online or not)

// IPIs (no-ops with a single CPU and in host mode).
//
// TLB shootdown: one request at a time (the initiator holds a lock while the others acknowledge). A CPU
// that spins for a lock with interrupts off cannot take the IPI, so every spin loop calls tlb_service()
// to carry out a pending request — two CPUs unmapping at once, or one waiting for the lock the other
// holds while it shoots down, would otherwise wait for each other.
void tlb_shootdown(uint64_t virt);                     // invlpg on every other CPU
void tlb_shootdown_range(uint64_t virt, uint64_t pages);  // a range (a full flush when it is large)
void tlb_flush_local(uint64_t virt, uint64_t pages);   // this CPU only
extern volatile uint64_t g_tlb_seq;
void tlb_service_slow();
inline void tlb_service() {
#if !NEKO_HOSTED
    if (g_percpu_ready && __atomic_load_n(&this_cpu()->tlb_seen, __ATOMIC_RELAXED) != __atomic_load_n(&g_tlb_seq, __ATOMIC_ACQUIRE))
        tlb_service_slow();
#endif
}
void halt_others();                 // NMI everyone else into a halt (used by the stop screen)
void kick(uint32_t index);          // wake an idle CPU so it picks up new work

}  // namespace neko::smp
