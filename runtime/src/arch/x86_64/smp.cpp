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
volatile uint64_t g_tlb_address = 0;
volatile uint32_t g_tlb_pending = 0;

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

void tlb_shootdown(uint64_t virt) {
    uint32_t others = online_count() - 1;
    if (others == 0) return;
    g_tlb_address = virt;
    __atomic_store_n(&g_tlb_pending, others, __ATOMIC_SEQ_CST);
    arch::send_ipi_all_others(arch::kVectorTlb, false);
    // Bounded wait: a CPU spinning with interrupts off must not hang us forever.
    for (uint64_t i = 0; i < 50000000 && __atomic_load_n(&g_tlb_pending, __ATOMIC_ACQUIRE); ++i) cpu::pause();
}

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
    __atomic_store_n(&c->online, true, __ATOMIC_RELEASE);
    sched::run_idle();
}

void smp_tlb_ipi() {
    __asm__ volatile("invlpg (%0)" : : "r"(smp::g_tlb_address) : "memory");
    __atomic_sub_fetch(&smp::g_tlb_pending, 1, __ATOMIC_ACQ_REL);
}

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
    arch::gdt_init(c->index);  // reloads GS, so the GS base is set afterwards
    arch::idt_load();
    smp::set_gs_base(c);
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
