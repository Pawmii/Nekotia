// Memory management: raw helpers, the kernel heap and the physical page allocator.
#pragma once

#include <neko/types.hpp>

namespace neko::mem {

inline void set(void* dst, uint8_t value, size_t n) { __builtin_memset(dst, value, n); }
inline void copy(void* dst, const void* src, size_t n) { __builtin_memcpy(dst, src, n); }
inline void move(void* dst, const void* src, size_t n) { __builtin_memmove(dst, src, n); }
inline int32_t compare(const void* a, const void* b, size_t n) { return __builtin_memcmp(a, b, n); }

constexpr size_t align_up(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }
constexpr size_t align_down(size_t v, size_t a) { return v & ~(a - 1); }

}  // namespace neko::mem

// ---------------------------------------------------------------------------
// Kernel heap: first-fit free list with splitting and coalescing (core/heap.cpp).
// On x86_64 it lives in its own virtual range and grows on demand through the VMM.
// Not thread-safe yet (single core, no preemption) — see ROADMAP phase 4.
namespace neko::heap {

constexpr size_t kAlignment = 16;

void add_region(void* base, size_t size);  // hand a block of memory to the heap

// Called when no free block is large enough; returns true after add_region()-ing at least `min_bytes`.
using Grower = bool (*)(size_t min_bytes);
void set_grower(Grower grower);

void* alloc(size_t size);
void* alloc_zeroed(size_t size);
void* realloc(void* block, size_t new_size);
void free(void* block);

size_t used_bytes();
size_t free_bytes();
size_t total_bytes();
size_t block_count();
bool check();  // validates every header; false on corruption

}  // namespace neko::heap

// ---------------------------------------------------------------------------
// Physical memory: buddy allocator over 4 KiB frames (core/pmm.cpp).
// Blocks of 2^order frames (order 0…10, 4 KiB…4 MiB) are physically contiguous and
// naturally aligned; freed buddies merge back. Pointers are virtual (HHDM) addresses.
namespace neko::pmm {

constexpr size_t kPageSize = 4096;
constexpr uint32_t kMaxOrder = 10;

void init(uint64_t hhdm_offset);
void add_region(void* virt_base, size_t size);  // usable RAM, any alignment

void* alloc_page();  // zero-filled, nullptr when out of memory
void free_page(void* page);

void* alloc_order(uint32_t order);  // 2^order contiguous, aligned, zero-filled frames
void free_order(void* block, uint32_t order);
void* alloc_contiguous(uint64_t pages);  // rounded up to a power of two
void free_contiguous(void* block, uint64_t pages);

uint64_t free_pages();
uint64_t total_pages();
uint64_t free_blocks(uint32_t order);  // statistics per order
uint32_t largest_free_order();         // -1u when empty

uint64_t virt_to_phys(const void* virt);
void* phys_to_virt(uint64_t phys);

}  // namespace neko::pmm

// ---------------------------------------------------------------------------
// Virtual memory (arch/x86_64/vmm.cpp): the kernel's own 4-level page tables.
//
//   0xffff800000000000  HHDM: all RAM, NX            (same offset as Limine)
//   0xffffc00000000000  kernel heap, grows upwards
//   0xffffd00000000000  free for VirtualMemory.Map (tests, drivers)
//   0xffffe00000000000  kernel stacks, each below an unmapped guard page
//   0xffffffff80000000  kernel image: .text RX, .rodata R, .data/.bss RW (NX)
namespace neko::vmm {

enum class PageFlags : uint32_t {
    None = 0,            // read-only, not executable
    Write = 1,
    Execute = 2,
    User = 4,
    Uncached = 8,        // MMIO
    WriteCombining = 16, // framebuffers
};
NEKO_ENUM_OPERATORS(PageFlags)

constexpr uint64_t kHeapBase = 0xffffc00000000000ull;
constexpr uint64_t kFreeBase = 0xffffd00000000000ull;
constexpr uint64_t kStackBase = 0xffffe00000000000ull;

bool ready();
bool map(uint64_t virt, uint64_t phys, PageFlags flags);  // 4 KiB page; false if already mapped
bool unmap(uint64_t virt);                                 // does not free the frame
bool protect(uint64_t virt, PageFlags flags);
uint64_t translate(uint64_t virt);                         // physical address or ~0 when unmapped

// Maps fresh frames for [virt, virt + size) — returns false when out of memory.
bool map_fresh(uint64_t virt, size_t size, PageFlags flags);

// Makes [phys, phys + size) reachable at HHDM + phys as uncached memory and returns that address.
void* map_mmio(uint64_t phys, size_t size);

// A kernel stack of `size` bytes with an unmapped guard page below; returns the top (initial rsp).
uint64_t create_stack(size_t size);
bool is_stack_guard(uint64_t address);

// Statistics for the shell.
struct Stats {
    uint64_t table_pages;      // pages used by page tables
    uint64_t heap_mapped;      // bytes mapped for the heap
    uint64_t stacks;           // kernel stacks created
    uint64_t kernel_phys;      // physical base of the kernel image
    uint64_t text_bytes, rodata_bytes, data_bytes;
    bool nx;                   // EFER.NXE active
    bool write_combining;      // PAT programmed (framebuffer WC)
    bool global_pages;         // CR4.PGE: kernel mappings survive CR3 switches
};
Stats stats();

}  // namespace neko::vmm
