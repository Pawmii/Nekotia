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
// Not thread-safe yet (single core, no preemption) — see ROADMAP phase 2.
namespace neko::heap {

constexpr size_t kAlignment = 16;

void add_region(void* base, size_t size);  // hand a block of memory to the heap

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
// Physical page allocator: stack of free 4 KiB frames threaded through the
// pages themselves (core/pmm.cpp). Pointers are virtual (HHDM) addresses.
namespace neko::pmm {

constexpr size_t kPageSize = 4096;

void init(uint64_t hhdm_offset);
void add_region(void* virt_base, size_t size);

void* alloc_page();  // zero-filled, nullptr when out of memory
void free_page(void* page);

uint64_t free_pages();
uint64_t total_pages();

uint64_t virt_to_phys(const void* virt);
void* phys_to_virt(uint64_t phys);

}  // namespace neko::pmm
