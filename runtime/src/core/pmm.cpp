// Buddy allocator for physical memory.
//
// Every usable RAM region becomes a zone. A zone keeps one metadata byte per
// frame (carved from the start of the region): kFreeHead | order for the first
// frame of a free block, 0 otherwise. Free blocks of each order are kept in a
// doubly linked list stored inside the blocks themselves (through the HHDM).
//
// One spin lock guards the lists and the metadata: every CPU allocates and frees (process exit on
// one CPU while another maps pages for its program). Zeroing happens outside it.
//
// Blocks are aligned to their size in *physical* addresses (buddy of frame n at
// order k is n ^ 2^k), so alloc_order(4) gives 64 KiB that is also 64 KiB aligned —
// what DMA engines want.
#include <neko/runtime.hpp>

namespace neko::pmm {
namespace {

constexpr uint8_t kFreeHead = 0x80;
constexpr uint32_t kMaxZones = 64;

struct FreeBlock {
    FreeBlock* next;
    FreeBlock* prev;
};

struct Zone {
    uint64_t first_pfn;  // physical frame number of meta[0]
    uint64_t pages;
    uint8_t* meta;
};

Zone g_zones[kMaxZones];
uint32_t g_zone_count = 0;
FreeBlock* g_free[kMaxOrder + 1];
uint64_t g_free_blocks[kMaxOrder + 1];
uint64_t g_free_pages = 0;
uint64_t g_total_pages = 0;
uint64_t g_hhdm = 0;
sync::SpinLock g_lock;

uint64_t pfn_of(const void* virt) { return virt_to_phys(virt) / kPageSize; }
FreeBlock* block_at(uint64_t pfn) { return static_cast<FreeBlock*>(phys_to_virt(pfn * kPageSize)); }

Zone* zone_of(uint64_t pfn) {
    for (uint32_t i = 0; i < g_zone_count; ++i)
        if (pfn >= g_zones[i].first_pfn && pfn < g_zones[i].first_pfn + g_zones[i].pages) return &g_zones[i];
    return nullptr;
}

void push(Zone& z, uint64_t pfn, uint32_t order) {
    FreeBlock* b = block_at(pfn);
    b->prev = nullptr;
    b->next = g_free[order];
    if (b->next) b->next->prev = b;
    g_free[order] = b;
    z.meta[pfn - z.first_pfn] = uint8_t(kFreeHead | order);
    ++g_free_blocks[order];
    g_free_pages += 1ull << order;
}

void unlink(Zone& z, uint64_t pfn, uint32_t order) {
    FreeBlock* b = block_at(pfn);
    if (b->prev) b->prev->next = b->next;
    else g_free[order] = b->next;
    if (b->next) b->next->prev = b->prev;
    z.meta[pfn - z.first_pfn] = 0;
    --g_free_blocks[order];
    g_free_pages -= 1ull << order;
}

void release(uint64_t pfn, uint32_t order) {
    Zone* z = zone_of(pfn);
    if (!z || order > kMaxOrder || (pfn & ((1ull << order) - 1)) != 0)
        crash::fail(crash::StopCode::HeapCorruption, "pmm: free of a block that was never allocated here");
    // Already free: the head of a free block, or inside a bigger one (any aligned head covering pfn).
    for (uint32_t k = 0; k <= kMaxOrder; ++k) {
        uint64_t head = pfn & ~((1ull << k) - 1);
        if (head < z->first_pfn) break;
        uint8_t m = z->meta[head - z->first_pfn];
        if ((m & kFreeHead) && (m & 0x7F) >= k)
            crash::fail(crash::StopCode::HeapCorruption, "pmm: double free of a page");
    }

    while (order < kMaxOrder) {
        uint64_t buddy = pfn ^ (1ull << order);
        if (buddy < z->first_pfn || buddy + (1ull << order) > z->first_pfn + z->pages) break;
        if (z->meta[buddy - z->first_pfn] != uint8_t(kFreeHead | order)) break;
        unlink(*z, buddy, order);
        pfn &= ~(1ull << order);
        ++order;
    }
    push(*z, pfn, order);
}

uint32_t order_for(uint64_t pages) {
    uint32_t order = 0;
    while ((1ull << order) < pages) ++order;
    return order;
}

}  // namespace

void init(uint64_t hhdm_offset) { g_hhdm = hhdm_offset; }

void add_region(void* virt_base, size_t size) {
    sync::ScopedLock guard(g_lock);
    if (g_zone_count == kMaxZones) return;
    uint64_t start = mem::align_up(virt_to_phys(virt_base), kPageSize) / kPageSize;
    uint64_t end = mem::align_down(virt_to_phys(virt_base) + size, kPageSize) / kPageSize;
    if (end <= start + 1) return;

    // Metadata: one byte per frame, stored in the first frames of the region.
    uint64_t frames = end - start;
    uint64_t meta_frames = (frames + kPageSize - 1) / kPageSize;
    if (meta_frames >= frames) return;

    Zone& z = g_zones[g_zone_count++];
    z.meta = static_cast<uint8_t*>(phys_to_virt(start * kPageSize));
    z.first_pfn = start + meta_frames;
    z.pages = frames - meta_frames;
    mem::set(z.meta, 0, z.pages);
    g_total_pages += z.pages;

    // Cover the zone with the largest naturally aligned blocks.
    uint64_t pfn = z.first_pfn, last = z.first_pfn + z.pages;
    while (pfn < last) {
        uint32_t order = kMaxOrder;
        while (order > 0 && ((pfn & ((1ull << order) - 1)) != 0 || pfn + (1ull << order) > last)) --order;
        push(z, pfn, order);
        pfn += 1ull << order;
    }
}

void* alloc_order(uint32_t order) {
    if (order > kMaxOrder) return nullptr;
    uint64_t pfn;
    {
        sync::ScopedLock guard(g_lock);
        uint32_t o = order;
        while (o <= kMaxOrder && !g_free[o]) ++o;
        if (o > kMaxOrder) return nullptr;
        pfn = pfn_of(g_free[o]);
        Zone* z = zone_of(pfn);
        unlink(*z, pfn, o);
        while (o > order) {  // split, keep the lower half, free the upper one
            --o;
            push(*z, pfn + (1ull << o), o);
        }
    }
    void* p = phys_to_virt(pfn * kPageSize);
    mem::set(p, 0, kPageSize << order);
    return p;
}

void free_order(void* block, uint32_t order) {
    if (!block) return;
    if ((reinterpret_cast<uintptr_t>(block) & (kPageSize - 1)) != 0)
        crash::fail(crash::StopCode::HeapCorruption, "pmm: free of an unaligned address");
    sync::ScopedLock guard(g_lock);
    release(pfn_of(block), order);
}

void* alloc_page() { return alloc_order(0); }
void free_page(void* page) { free_order(page, 0); }
void* alloc_contiguous(uint64_t pages) { return pages ? alloc_order(order_for(pages)) : nullptr; }
void free_contiguous(void* block, uint64_t pages) { free_order(block, order_for(pages)); }

uint64_t free_pages() { return g_free_pages; }
uint64_t total_pages() { return g_total_pages; }
uint64_t free_blocks(uint32_t order) { return order <= kMaxOrder ? g_free_blocks[order] : 0; }

uint32_t largest_free_order() {
    sync::ScopedLock guard(g_lock);
    for (uint32_t o = kMaxOrder + 1; o-- > 0;)
        if (g_free[o]) return o;
    return ~0u;
}

uint64_t virt_to_phys(const void* virt) { return reinterpret_cast<uint64_t>(virt) - g_hhdm; }
void* phys_to_virt(uint64_t phys) { return reinterpret_cast<void*>(phys + g_hhdm); }

}  // namespace neko::pmm
