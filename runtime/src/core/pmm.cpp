// Physical page frame allocator: a LIFO stack of free 4 KiB frames.
// The "next" pointer of each free frame is stored inside the frame itself,
// so the allocator needs no memory of its own. O(1) alloc and free.
// ROADMAP phase 2 replaces it with a bitmap/buddy allocator for contiguous runs.
#include <neko/runtime.hpp>

namespace neko::pmm {
namespace {

struct FreeFrame {
    FreeFrame* next;
};

FreeFrame* g_free = nullptr;
uint64_t g_free_count = 0;
uint64_t g_total = 0;
uint64_t g_hhdm = 0;

}  // namespace

void init(uint64_t hhdm_offset) { g_hhdm = hhdm_offset; }

void add_region(void* virt_base, size_t size) {
    uintptr_t start = mem::align_up(reinterpret_cast<uintptr_t>(virt_base), kPageSize);
    uintptr_t end = mem::align_down(reinterpret_cast<uintptr_t>(virt_base) + size, kPageSize);
    for (uintptr_t p = start; p + kPageSize <= end; p += kPageSize) {
        auto* f = reinterpret_cast<FreeFrame*>(p);
        f->next = g_free;
        g_free = f;
        ++g_free_count;
        ++g_total;
    }
}

void* alloc_page() {
    FreeFrame* f = g_free;
    if (!f) return nullptr;
    g_free = f->next;
    --g_free_count;
    mem::set(f, 0, kPageSize);
    return f;
}

void free_page(void* page) {
    if (!page) return;
    if ((reinterpret_cast<uintptr_t>(page) & (kPageSize - 1)) != 0) panic("pmm: free_page of an unaligned address");
    auto* f = static_cast<FreeFrame*>(page);
    f->next = g_free;
    g_free = f;
    ++g_free_count;
}

uint64_t free_pages() { return g_free_count; }
uint64_t total_pages() { return g_total; }

uint64_t virt_to_phys(const void* virt) { return reinterpret_cast<uint64_t>(virt) - g_hhdm; }
void* phys_to_virt(uint64_t phys) { return reinterpret_cast<void*>(phys + g_hhdm); }

}  // namespace neko::pmm
