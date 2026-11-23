// x86_64-internal interfaces between boot.cpp and the arch subsystems.
#pragma once

#include <neko/runtime.hpp>

namespace neko::arch {

constexpr uint16_t kKernelCode = 0x08;
constexpr uint16_t kKernelData = 0x10;
constexpr uint16_t kUserData = 0x18 | 3;  // selectors with RPL 3
constexpr uint16_t kUserCode = 0x20 | 3;
constexpr uint16_t kTss = 0x28;

// Interrupt vectors
constexpr uint8_t kVectorTimer = 0x20;
constexpr uint8_t kVectorIrqBase = 0x30;  // ISA IRQ n → 0x30 + n
constexpr uint8_t kVectorSpurious = 0xFF;

void gdt_init(uint32_t cpu);  // own GDT + TSS with IST stacks for #DF, NMI, #MC (per CPU)
void idt_init();              // builds the shared IDT and loads it
void idt_load();              // loads it on an AP
void set_kernel_stack(uint32_t cpu, uint64_t top);  // TSS.rsp0: stack for interrupts from ring 3

// User mode (usermode.cpp, syscall.S)
void syscall_init_this_cpu();  // EFER.SCE, STAR/LSTAR/SFMASK

// Interrupt vectors for inter-processor interrupts
constexpr uint8_t kVectorKick = 0xF0;      // reschedule
constexpr uint8_t kVectorTlb = 0xF1;       // TLB shootdown

// Local APIC helpers (apic.cpp)
void lapic_init_this_cpu();                 // APs: enable LAPIC + periodic timer (BSP calibration)
uint32_t lapic_id();
void send_ipi(uint32_t lapic_id, uint8_t vector);
void send_ipi_all_others(uint8_t vector, bool nmi);

// SMP (smp.cpp)
void smp_start(void* mp_response);          // limine_mp_response*
uint32_t smp_started();                     // CPUs smp_start tried to bring up (incl. the BSP)
void smp_tlb_ipi();                         // handler of kVectorTlb
void percpu_init_bsp();

struct MadtInfo {
    uint64_t lapic_phys = 0xFEE00000;
    struct IoApic {
        uint8_t id;
        uint64_t phys;
        uint32_t gsi_base;
    } ioapics[4];
    uint32_t ioapic_count = 0;
    struct Override {
        uint8_t source;
        uint32_t gsi;
        uint16_t flags;
    } overrides[16];
    uint32_t override_count = 0;
    uint32_t cpu_count = 0;
};

bool acpi_init(void* rsdp, uint64_t hhdm, MadtInfo& out);
void apic_init(const MadtInfo& madt);  // masks the 8259, enables the LAPIC, starts the 1 kHz timer
void irq_dispatch(uint8_t vector);

}  // namespace neko::arch

namespace neko::vmm {

struct MemoryRange {
    uint64_t base;
    uint64_t length;
    bool framebuffer;
};

// Builds the kernel's page tables (HHDM for `ranges`, the kernel image with per-section
// permissions) and switches CR3. Needs a working pmm.
void init(uint64_t hhdm, uint64_t kernel_phys, const MemoryRange* ranges, size_t count);

// Heap grower installed after init.
bool grow_heap(size_t min_bytes);

// APs: same CPU features (NX, PAT, WP) and CR3 as the BSP.
void load_on_this_cpu();
uint64_t root_phys();

}  // namespace neko::vmm
