// Minimal page-table editing on top of the tables Limine built.
// Base revision 3 no longer maps MMIO into the HHDM, so the LAPIC, IOAPIC and
// (sometimes) ACPI tables have to be mapped by hand. A full VMM with our own
// tables is ROADMAP phase 3.
#include "arch.hpp"

namespace neko::vmm {
namespace {

constexpr uint64_t kPresent = 1ull << 0;
constexpr uint64_t kWritable = 1ull << 1;
constexpr uint64_t kWriteThrough = 1ull << 3;
constexpr uint64_t kCacheDisable = 1ull << 4;
constexpr uint64_t kHuge = 1ull << 7;
constexpr uint64_t kAddressMask = 0x000FFFFFFFFFF000ull;

uint64_t* table(uint64_t entry) { return static_cast<uint64_t*>(pmm::phys_to_virt(entry & kAddressMask)); }

// Returns the leaf entry for `virt` (creating intermediate tables), or nullptr if a huge page already maps it.
uint64_t* leaf(uint64_t virt) {
    uint64_t* t = table(cpu::read_cr3());
    for (int shift = 39; shift >= 21; shift -= 9) {
        uint64_t& e = t[(virt >> shift) & 0x1FF];
        if (e & kPresent) {
            if (e & kHuge) return nullptr;
            t = table(e);
            continue;
        }
        void* page = pmm::alloc_page();
        if (!page) crash::fail(crash::StopCode::OutOfMemory, "vmm: no free page for a page table");
        e = pmm::virt_to_phys(page) | kPresent | kWritable;
        t = static_cast<uint64_t*>(page);
    }
    return &t[(virt >> 12) & 0x1FF];
}

}  // namespace

void* map_mmio(uint64_t phys, size_t size) {
    uint64_t hhdm = reinterpret_cast<uint64_t>(pmm::phys_to_virt(0));
    uint64_t first = phys & ~0xFFFull;
    uint64_t last = (phys + size + 0xFFF) & ~0xFFFull;
    for (uint64_t p = first; p < last; p += 0x1000) {
        uint64_t* e = leaf(hhdm + p);
        if (!e || (*e & kPresent)) continue;  // already mapped (HHDM huge page or earlier call)
        *e = p | kPresent | kWritable | kWriteThrough | kCacheDisable;
        __asm__ volatile("invlpg (%0)" : : "r"(hhdm + p) : "memory");
    }
    return reinterpret_cast<void*>(hhdm + phys);
}

}  // namespace neko::vmm
