// Our own GDT and TSS (Limine's GDT has no TSS). The TSS gives the double fault,
// NMI and machine check handlers their own stacks (IST), so even a kernel stack
// overflow produces a readable report instead of a triple fault.
//
// Layout (ready for syscall/sysret later):
//   0x00 null   0x08 kernel code   0x10 kernel data   0x18 user data   0x20 user code   0x28 TSS (16 bytes)
#include "arch.hpp"

#include <neko/smp.hpp>

namespace neko::arch {
namespace {

struct __attribute__((packed)) Tss {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
};
static_assert(sizeof(Tss) == 104, "TSS layout");

struct __attribute__((packed)) GdtPointer {
    uint16_t limit;
    uint64_t base;
};

constexpr size_t kIstStackSize = 16 * 1024;

// One GDT/TSS per CPU: the TSS holds that CPU's IST stacks.
alignas(16) uint64_t g_gdts[smp::kMaxCpus][7];
alignas(16) Tss g_tsses[smp::kMaxCpus];
alignas(16) uint8_t g_ist_stacks[smp::kMaxCpus][3][kIstStackSize];

}  // namespace

void gdt_init(uint32_t cpu) {
    uint64_t* g_gdt = g_gdts[cpu];
    Tss& g_tss = g_tsses[cpu];
    uint8_t (*ist)[kIstStackSize] = g_ist_stacks[cpu];
    g_gdt[0] = 0;
    g_gdt[1] = 0x00AF9A000000FFFF;  // kernel code, 64-bit
    g_gdt[2] = 0x00CF92000000FFFF;  // kernel data
    g_gdt[3] = 0x00CFF2000000FFFF;  // user data (DPL 3)
    g_gdt[4] = 0x00AFFA000000FFFF;  // user code, 64-bit (DPL 3)

    for (int i = 0; i < 3; ++i) g_tss.ist[i] = reinterpret_cast<uint64_t>(ist[i] + kIstStackSize);
    g_tss.iomap_base = sizeof(Tss);

    uint64_t base = reinterpret_cast<uint64_t>(&g_tss);
    uint64_t limit = sizeof(Tss) - 1;
    g_gdt[5] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ull << 40) | (((limit >> 16) & 0xF) << 48) |
               (((base >> 24) & 0xFF) << 56);
    g_gdt[6] = base >> 32;

    GdtPointer ptr = {sizeof(g_gdts[0]) - 1, reinterpret_cast<uint64_t>(g_gdt)};
    __asm__ volatile(
        "lgdt %0\n\t"
        "pushq %1\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n"
        "1:\n\t"
        "movw %2, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        :
        : "m"(ptr), "i"(uint64_t(kKernelCode)), "i"(kKernelData)
        : "rax", "memory");
    __asm__ volatile("ltr %0" : : "r"(kTss));
}

}  // namespace neko::arch
