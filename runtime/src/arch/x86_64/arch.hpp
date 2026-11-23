// x86_64-internal interfaces between boot.cpp and the arch subsystems.
#pragma once

#include <neko/runtime.hpp>

namespace neko::arch {

constexpr uint16_t kKernelCode = 0x08;
constexpr uint16_t kKernelData = 0x10;
constexpr uint16_t kTss = 0x28;

// Interrupt vectors
constexpr uint8_t kVectorTimer = 0x20;
constexpr uint8_t kVectorIrqBase = 0x30;  // ISA IRQ n → 0x30 + n
constexpr uint8_t kVectorSpurious = 0xFF;

void gdt_init();  // own GDT + TSS with IST stacks for #DF, NMI, #MC
void idt_init();  // exception + IRQ gates

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

}  // namespace neko::vmm
