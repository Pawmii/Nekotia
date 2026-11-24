// Multiprocessor support: per-CPU records (GS base), starting the application
// processors through the Limine MP request, and inter-processor interrupts.
//
// AP start-up: Limine parks every AP in a loop until its goto_address is written.
// neko_ap_entry (entry.S) enables SSE, then neko_ap_main loads our page tables, GDT/TSS,
// IDT and GS base, starts the local APIC timer, moves to a guarded kernel stack and
// becomes that CPU's idle thread — from then on the scheduler hands it work.
#include <limine.h>

#include "arch.hpp"

#include <neko/sched.hpp>
#include <neko/smp.hpp>

extern "C" void neko_ap_entry(limine_mp_info* info);

namespace neko::smp {

Cpu g_cpus[kMaxCpus];
bool g_percpu_ready = false;

uint32_t g_started = 1;
// The current TLB shootdown request (valid while g_tlb_pending > 0; one at a time under g_tlb_lock).
volatile uint64_t g_tlb_seq = 0;
volatile uint64_t g_tlb_start = 0;
volatile uint64_t g_tlb_pages = 0;
volatile uint32_t g_tlb_pending = 0;
volatile uint32_t g_tlb_lock = 0;

void set_gs_base(Cpu* c) {
    uint64_t v = reinterpret_cast<uint64_t>(c);
    __asm__ volatile("wrmsr" : : "c"(0xC0000101), "a"(uint32_t(v)), "d"(uint32_t(v >> 32)));
}

uint32_t online_count() {
    uint32_t n = 0;
    for (uint32_t i = 0; i < kMaxCpus; ++i) n += g_cpus[i].online ? 1 : 0;
    return n ? n : 1;
}

uint32_t cpu_count() { return g_started; }

void tlb_flush_local(uint64_t virt, uint64_t pages) {
    if (pages <= 64) {
        for (uint64_t i = 0; i < pages; ++i) __asm__ volatile("invlpg (%0)" : : "r"(virt + i * 4096) : "memory");
        return;
    }
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    if (virt >= 0xffff800000000000ull && (cr4 & (1ull << 7))) {
        // kernel pages are global: only toggling CR4.PGE drops them
        __asm__ volatile("mov %0, %%cr4" : : "r"(cr4 & ~(1ull << 7)) : "memory");
        __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
        return;
    }
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) : : "memory");
}

void tlb_service_slow() {
    Cpu* c = this_cpu();
    uint64_t seq = __atomic_load_n(&g_tlb_seq, __ATOMIC_ACQUIRE);
    uint64_t flags = cpu::save_and_disable_interrupts();  // not twice for one request (IPI on top of a spin loop)
    if (c->tlb_seen != seq) {
        tlb_flush_local(g_tlb_start, g_tlb_pages);
        c->tlb_seen = seq;
        __atomic_sub_fetch(&g_tlb_pending, 1, __ATOMIC_ACQ_REL);
    }
    cpu::restore_interrupts(flags);
}

void tlb_shootdown_range(uint64_t virt, uint64_t pages) {
    if (!g_percpu_ready || pages == 0) return;
    uint32_t others = online_count() - 1;
    if (others == 0) return;
    // One request at a time; while waiting for our turn, carry out the request of whoever has it.
    while (__atomic_exchange_n(&g_tlb_lock, 1u, __ATOMIC_ACQUIRE)) {
        tlb_service();
        cpu::pause();
    }
    tlb_service();  // a request may have completed just as we got the lock: nothing pending for us now
    g_tlb_start = virt;
    g_tlb_pages = pages;
    __atomic_store_n(&g_tlb_pending, others, __ATOMIC_SEQ_CST);
    uint64_t seq = __atomic_add_fetch(&g_tlb_seq, 1, __ATOMIC_SEQ_CST);
    this_cpu()->tlb_seen = seq;  // the caller has flushed its own TLB
    arch::send_ipi_all_others(arch::kVectorTlb, false);
    // Bounded wait: a CPU stuck with interrupts off outside any spin loop must not hang us for ever.
    for (uint64_t i = 0; i < 50000000 && __atomic_load_n(&g_tlb_pending, __ATOMIC_ACQUIRE); ++i) cpu::pause();
    __atomic_store_n(&g_tlb_lock, 0u, __ATOMIC_RELEASE);
}

void tlb_shootdown(uint64_t virt) { tlb_shootdown_range(virt, 1); }

void halt_others() {
    if (g_started > 1) arch::send_ipi_all_others(0, true);
}

void kick(uint32_t index) {
    if (index < kMaxCpus && g_cpus[index].online) arch::send_ipi(g_cpus[index].lapic_id, arch::kVectorKick);
}

}  // namespace neko::smp

namespace neko::arch {

[[noreturn]] void ap_on_kernel_stack();
[[noreturn]] void ap_on_kernel_stack() {
    smp::Cpu* c = smp::this_cpu();
    sched::cpu_online();
    c->tlb_seen = __atomic_load_n(&smp::g_tlb_seq, __ATOMIC_ACQUIRE);  // earlier requests did not count this CPU
    __atomic_store_n(&c->online, true, __ATOMIC_RELEASE);
    sched::run_idle();
}

uint32_t smp_started() { return smp::g_started ? smp::g_started : 1; }

void smp_tlb_ipi() { smp::tlb_service(); }

void percpu_init_bsp() {
    smp::Cpu& c = smp::g_cpus[0];
    c.self = &c;
    c.index = 0;
    smp::set_gs_base(&c);
    smp::g_percpu_ready = true;
    c.online = true;
}

void smp_start(void* response) {
    auto* mp = static_cast<limine_mp_response*>(response);
    smp::g_cpus[0].lapic_id = lapic_id();
    if (!mp) return;

    uint32_t next = 1;
    for (uint64_t i = 0; i < mp->cpu_count && next < smp::kMaxCpus; ++i) {
        limine_mp_info* info = mp->cpus[i];
        if (info->lapic_id == mp->bsp_lapic_id) continue;
        smp::Cpu& c = smp::g_cpus[next];
        c.self = &c;
        c.index = next;
        c.lapic_id = info->lapic_id;
        c.stack_top = vmm::create_stack(16 * 1024);
        if (!c.stack_top) break;
        info->extra_argument = reinterpret_cast<uint64_t>(&c);
        __atomic_store_n(&info->goto_address, &neko_ap_entry, __ATOMIC_RELEASE);
        ++next;
    }
    smp::g_started = next;

    uint64_t deadline = time::uptime_ms() + 1000;
    while (smp::online_count() < next && time::uptime_ms() < deadline) cpu::halt();
}

}  // namespace neko::arch

// Runs on the stack Limine gave the AP, with SSE already on (entry.S).
extern "C" [[noreturn]] void neko_ap_main(limine_mp_info* info) {
    using namespace neko;
    auto* c = reinterpret_cast<smp::Cpu*>(info->extra_argument);
    vmm::load_on_this_cpu();
    arch::enable_smep_smap();
    arch::gdt_init(c->index);  // reloads GS, so the GS base is set afterwards
    arch::idt_load();
    smp::set_gs_base(c);
    arch::syscall_init_this_cpu();
    arch::lapic_init_this_cpu();

    uint64_t top = c->stack_top;
    __asm__ volatile(
        "mov %0, %%rsp\n\t"
        "xor %%ebp, %%ebp\n\t"
        "call *%1\n\t"
        "ud2"
        :
        : "r"(top), "r"(&arch::ap_on_kernel_stack)
        : "memory");
    __builtin_unreachable();
}
