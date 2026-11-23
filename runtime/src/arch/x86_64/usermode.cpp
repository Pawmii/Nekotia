// Ring 3 support: the syscall MSRs and the per-switch state the CPU needs for user threads.
//
//   STAR    [47:32] = 0x08 → syscall loads CS 0x08 / SS 0x10
//           [63:48] = 0x10 → sysretq loads CS 0x10 + 16 = 0x20|3 and SS 0x10 + 8 = 0x18|3 (gdt.cpp)
//   LSTAR   neko_syscall_entry (syscall.S)
//   SFMASK  IF, DF, TF, AC, NT, IOPL cleared on entry
#include "arch.hpp"

#include <neko/process.hpp>
#include <neko/sched.hpp>
#include <neko/smp.hpp>

#include <stddef.h>

extern "C" void neko_syscall_entry();
extern "C" [[noreturn]] void neko_enter_user(uint64_t rip, uint64_t rsp, uint64_t arg0, uint64_t arg1);

static_assert(offsetof(neko::smp::Cpu, kernel_rsp) == 8, "syscall.S reads gs:8");
static_assert(offsetof(neko::smp::Cpu, user_rsp) == 16, "syscall.S writes gs:16");
static_assert(sizeof(neko::proc::SyscallFrame) == 20 * 8, "syscall.S pushes 20 registers");

extern "C" {
uint8_t neko_smap_enabled = 0;  // read by neko_user_copy (syscall.S): stac/clac only when supported
}

namespace neko::arch {
namespace {

bool g_available = false;

void wrmsr(uint32_t msr, uint64_t v) { __asm__ volatile("wrmsr" : : "c"(msr), "a"(uint32_t(v)), "d"(uint32_t(v >> 32))); }

uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (uint64_t(hi) << 32) | lo;
}

}  // namespace

void syscall_init_this_cpu() {
    constexpr uint32_t kEfer = 0xC0000080, kStar = 0xC0000081, kLstar = 0xC0000082, kSfmask = 0xC0000084;
    wrmsr(kEfer, rdmsr(kEfer) | 1);  // SCE: syscall/sysret
    wrmsr(kStar, (uint64_t(kKernelData) << 48) | (uint64_t(kKernelCode) << 32));
    wrmsr(kLstar, reinterpret_cast<uint64_t>(&neko_syscall_entry));
    wrmsr(kSfmask, 0x47700);
    wrmsr(0xC0000102, 0);  // KERNEL_GS_BASE: the user GS base swapgs exchanges with ours
    g_available = true;
}

bool user_mode_available() { return g_available; }

bool g_smep = false;
bool g_smap = false;

void enable_smep_smap() {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0), "c"(0));
    if (a < 7) return;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    if (b & (1u << 7)) cr4 |= 1ull << 20;   // SMEP: ring 0 never executes user pages
    if (b & (1u << 20)) cr4 |= 1ull << 21;  // SMAP: ring 0 touches user pages only in neko_user_copy
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
    g_smep = b & (1u << 7);
    g_smap = b & (1u << 20);
    neko_smap_enabled = g_smap ? 1 : 0;
}

bool smep_enabled() { return g_smep; }
bool smap_enabled() { return g_smap; }

void enter_user(uint64_t rip, uint64_t rsp, uint64_t arg0, uint64_t arg1) { neko_enter_user(rip, rsp, arg0, arg1); }

}  // namespace neko::arch

// Scheduler hook, called with the scheduler lock held right before switching to `next`:
// interrupts and system calls from ring 3 must land on next's kernel stack, and user threads
// need their own page tables (kernel threads use the kernel's, which every process shares).
void neko_sched_switch_to(neko::sched::Thread* next) {
    using namespace neko;
    smp::Cpu* c = smp::this_cpu();
    c->kernel_rsp = next->stack_top;
    arch::set_kernel_stack(c->index, next->stack_top);
    uint64_t want = next->process ? next->process->pml4_phys : vmm::root_phys();
    if (cpu::read_cr3() != want) __asm__ volatile("mov %0, %%cr3" : : "r"(want) : "memory");
}
