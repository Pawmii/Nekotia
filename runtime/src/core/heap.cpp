// Kernel heap: address-ordered doubly linked list of blocks, first fit,
// split on allocation, coalesce with physical neighbours on free.
//
//   [Block hdr | payload ............][Block hdr | payload ...]...
//
// Every header carries a magic value so double frees, frees of foreign
// pointers and most overruns of the previous block are caught.
#include <neko/runtime.hpp>

namespace neko::heap {
namespace {

constexpr uint32_t kMagicUsed = 0x4F4B454E;  // "NEKO"
constexpr uint32_t kMagicFree = 0x45455246;  // "FREE"

struct alignas(kAlignment) Block {
    uint32_t magic;
    uint32_t reserved;
    size_t size;  // payload bytes, multiple of kAlignment
    Block* prev;  // address order
    Block* next;
};
static_assert(sizeof(Block) % kAlignment == 0, "header must keep payloads aligned");

constexpr size_t kMinSplit = sizeof(Block) + kAlignment;

Block* g_head = nullptr;
size_t g_total = 0;  // payload bytes in all blocks
size_t g_used = 0;   // payload bytes in used blocks
size_t g_blocks = 0;

uint8_t* payload(Block* b) { return reinterpret_cast<uint8_t*>(b + 1); }
Block* header_of(void* p) { return static_cast<Block*>(p) - 1; }
bool touches(Block* a, Block* b) { return payload(a) + a->size == reinterpret_cast<uint8_t*>(b); }

// Absorb b->next into b. Both must be free and adjacent.
void merge_next(Block* b) {
    Block* n = b->next;
    b->size += sizeof(Block) + n->size;
    b->next = n->next;
    if (n->next) n->next->prev = b;
    n->magic = 0;
    g_total += sizeof(Block);
    --g_blocks;
}

void coalesce(Block* b) {
    if (b->next && b->next->magic == kMagicFree && touches(b, b->next)) merge_next(b);
    if (b->prev && b->prev->magic == kMagicFree && touches(b->prev, b)) merge_next(b->prev);
}

Block* validate(void* p, const char* who) {
    if ((reinterpret_cast<uintptr_t>(p) & (kAlignment - 1)) != 0) crash::fail(crash::StopCode::HeapCorruption, who);
    Block* b = header_of(p);
    if (b->magic == kMagicFree) crash::fail(crash::StopCode::HeapCorruption, "heap: double free");
    if (b->magic != kMagicUsed) crash::fail(crash::StopCode::HeapCorruption, who);
    return b;
}

}  // namespace

void add_region(void* base, size_t size) {
    uintptr_t start = mem::align_up(reinterpret_cast<uintptr_t>(base), kAlignment);
    uintptr_t end = mem::align_down(reinterpret_cast<uintptr_t>(base) + size, kAlignment);
    if (end <= start || end - start < kMinSplit) return;

    Block* b = reinterpret_cast<Block*>(start);
    b->magic = kMagicFree;
    b->reserved = 0;
    b->size = end - start - sizeof(Block);
    b->prev = b->next = nullptr;

    // Insert sorted by address.
    Block* after = nullptr;
    for (Block* it = g_head; it && it < b; it = it->next) after = it;
    if (after) {
        b->next = after->next;
        b->prev = after;
        if (after->next) after->next->prev = b;
        after->next = b;
    } else {
        b->next = g_head;
        if (g_head) g_head->prev = b;
        g_head = b;
    }

    g_total += b->size;
    ++g_blocks;
    coalesce(b);
}

void* alloc(size_t size) {
    if (size == 0) size = 1;
    size = mem::align_up(size, kAlignment);

    for (Block* b = g_head; b; b = b->next) {
        if (b->magic != kMagicFree || b->size < size) continue;

        if (b->size >= size + kMinSplit) {
            Block* rest = reinterpret_cast<Block*>(payload(b) + size);
            rest->magic = kMagicFree;
            rest->reserved = 0;
            rest->size = b->size - size - sizeof(Block);
            rest->prev = b;
            rest->next = b->next;
            if (b->next) b->next->prev = rest;
            b->next = rest;
            b->size = size;
            g_total -= sizeof(Block);
            ++g_blocks;
        }

        b->magic = kMagicUsed;
        g_used += b->size;
        return payload(b);
    }
    return nullptr;  // out of memory — ROADMAP phase 2: grow through the VMM
}

void* alloc_zeroed(size_t size) {
    void* p = alloc(size);
    if (p) mem::set(p, 0, header_of(p)->size);
    return p;
}

void free(void* p) {
    if (!p) return;
    Block* b = validate(p, "heap: free of an invalid pointer (or a corrupted header)");
    b->magic = kMagicFree;
    g_used -= b->size;
    coalesce(b);
}

void* realloc(void* p, size_t new_size) {
    if (!p) return alloc(new_size);
    if (new_size == 0) {
        free(p);
        return nullptr;
    }
    Block* b = validate(p, "heap: realloc of an invalid pointer (or a corrupted header)");
    size_t want = mem::align_up(new_size, kAlignment);
    if (b->size >= want) return p;

    // Grow in place when the next block is free and adjacent.
    if (b->next && b->next->magic == kMagicFree && touches(b, b->next) &&
        b->size + sizeof(Block) + b->next->size >= want) {
        g_used -= b->size;
        merge_next(b);
        g_used += b->size;
        return p;
    }

    void* q = alloc(new_size);
    if (!q) return nullptr;
    mem::copy(q, p, b->size);
    free(p);
    return q;
}

size_t used_bytes() { return g_used; }
size_t free_bytes() { return g_total - g_used; }
size_t total_bytes() { return g_total; }
size_t block_count() { return g_blocks; }

bool check() {
    size_t blocks = 0, total = 0, used = 0;
    for (Block* b = g_head; b; b = b->next) {
        if (b->magic != kMagicUsed && b->magic != kMagicFree) return false;
        if (b->next && (b->next <= b || b->next->prev != b)) return false;
        if ((b->size & (kAlignment - 1)) != 0) return false;
        if (b->magic == kMagicUsed) used += b->size;
        total += b->size;
        ++blocks;
    }
    return blocks == g_blocks && total == g_total && used == g_used;
}

}  // namespace neko::heap
