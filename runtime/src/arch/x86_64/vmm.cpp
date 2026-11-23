// The kernel's own page tables (see the address-space layout in neko/memory.hpp).
//
// vmm::init builds a fresh PML4:
//   * HHDM: every RAM range from the memory map, 2 MiB pages where possible, NX,
//     the framebuffer write-combining (PAT entry 4)
//   * the kernel image with per-section permissions (.text RX, .rodata R, data RW, NX)
// then enables EFER.NXE and CR0.WP and switches CR3. Limine's tables are not used afterwards.
#include "arch.hpp"

#include <neko/sched.hpp>
#include <neko/smp.hpp>

extern "C" char __kernel_start[], __text_start[], __text_end[], __rodata_start[], __rodata_end[];
extern "C" char __data_start[], __data_end[], __symbols_start[], __symbols_end[];

namespace neko::vmm {
namespace {

constexpr uint64_t kPresent = 1ull << 0;
constexpr uint64_t kWritable = 1ull << 1;
constexpr uint64_t kUser = 1ull << 2;
constexpr uint64_t kWriteThrough = 1ull << 3;
constexpr uint64_t kCacheDisable = 1ull << 4;
constexpr uint64_t kHuge = 1ull << 7;     // PDE/PDPTE: large page
constexpr uint64_t kPat4K = 1ull << 7;    // PTE: PAT index bit
constexpr uint64_t kPat2M = 1ull << 12;   // PDE with kHuge: PAT index bit
constexpr uint64_t kNoExecute = 1ull << 63;
constexpr uint64_t kAddress = 0x000FFFFFFFFFF000ull;
constexpr uint64_t kPage = 0x1000;
constexpr uint64_t kLargePage = 0x200000;
constexpr uint64_t kKernelVirtualBase = 0xffffffff80000000ull;

constexpr uint64_t kHeapLimit = 64ull << 30;  // 64 GiB of address space for the heap
constexpr uint64_t kHeapChunk = 1ull << 20;   // grow by at least 1 MiB

uint64_t* g_pml4 = nullptr;
uint64_t g_hhdm = 0;
bool g_ready = false;
bool g_nx = false;
bool g_wc = false;
uint64_t g_table_pages = 0;
uint64_t g_heap_end = kHeapBase;
uint64_t g_stack_next = kStackBase;
uint64_t g_stack_guards[64];
uint32_t g_stack_count = 0;
uint64_t g_kernel_phys = 0;
sync::SpinLock g_vm_lock;  // recursive: the heap grower maps pages while holding it

bool has(PageFlags f, PageFlags bit) { return (f & bit) != PageFlags::None; }

uint64_t* table(uint64_t entry) { return static_cast<uint64_t*>(pmm::phys_to_virt(entry & kAddress)); }

uint64_t* root() { return g_pml4 ? g_pml4 : table(cpu::read_cr3()); }

uint64_t* new_table() {
    void* p = pmm::alloc_page();
    if (!p) crash::fail(crash::StopCode::OutOfMemory, "vmm: no free page for a page table");
    ++g_table_pages;
    return static_cast<uint64_t*>(p);
}

uint64_t leaf_flags(PageFlags f, bool large) {
    uint64_t v = kPresent;
    if (has(f, PageFlags::Write)) v |= kWritable;
    if (has(f, PageFlags::User)) v |= kUser;
    if (!has(f, PageFlags::Execute) && g_nx) v |= kNoExecute;
    if (has(f, PageFlags::Uncached)) v |= kCacheDisable | kWriteThrough;
    else if (has(f, PageFlags::WriteCombining) && g_wc) v |= large ? kPat2M : kPat4K;
    return v;
}

// Entry at `level` (3 = PML4 … 0 = PT) for `virt`; creates missing tables down to `level`.
// Returns nullptr when a large page already covers the address or `create` is false and a table is missing.
uint64_t* entry(uint64_t virt, int level, bool create) {
    uint64_t* t = root();
    for (int l = 3; l > level; --l) {
        uint64_t& e = t[(virt >> (12 + 9 * l)) & 0x1FF];
        if (!(e & kPresent)) {
            if (!create) return nullptr;
            uint64_t* n = new_table();
            e = pmm::virt_to_phys(n) | kPresent | kWritable | kUser;  // permissions are decided by the leaf
            t = n;
            continue;
        }
        if (e & kHuge) return nullptr;
        t = table(e);
    }
    return &t[(virt >> (12 + 9 * level)) & 0x1FF];
}

void invalidate(uint64_t virt) { __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory"); }

uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (uint64_t(hi) << 32) | lo;
}

void wrmsr(uint32_t msr, uint64_t v) { __asm__ volatile("wrmsr" : : "c"(msr), "a"(uint32_t(v)), "d"(uint32_t(v >> 32))); }

void cpuid(uint32_t leaf, uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) {
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(leaf), "c"(0));
}

void enable_cpu_features() {
    uint32_t a, b, c, d;
    cpuid(0x80000000, a, b, c, d);
    if (a >= 0x80000001) {
        cpuid(0x80000001, a, b, c, d);
        if (d & (1u << 20)) {  // NX
            wrmsr(0xC0000080, rdmsr(0xC0000080) | (1ull << 11));
            g_nx = true;
        }
    }
    cpuid(1, a, b, c, d);
    if (d & (1u << 16)) {  // PAT: keep the reset layout but make entry 4 write-combining
        __asm__ volatile("wbinvd" ::: "memory");
        wrmsr(0x277, 0x0007040100070406ull);
        __asm__ volatile("wbinvd" ::: "memory");
        g_wc = true;
    }
}

void map_range_hhdm(uint64_t base, uint64_t length, bool framebuffer) {
    uint64_t start = base & ~(kPage - 1);
    uint64_t end = (base + length + kPage - 1) & ~(kPage - 1);
    PageFlags flags = PageFlags::Write | (framebuffer ? PageFlags::WriteCombining : PageFlags::None);
    for (uint64_t p = start; p < end;) {
        if ((p & (kLargePage - 1)) == 0 && p + kLargePage <= end) {
            if (uint64_t* pde = entry(g_hhdm + p, 1, true); pde && !(*pde & kPresent)) *pde = p | leaf_flags(flags, true) | kHuge;
            p += kLargePage;
        } else {
            if (uint64_t* pte = entry(g_hhdm + p, 0, true); pte && !(*pte & kPresent)) *pte = p | leaf_flags(flags, false);
            p += kPage;
        }
    }
}

void map_kernel_section(const char* from, const char* to, PageFlags flags) {
    uint64_t start = reinterpret_cast<uint64_t>(from) & ~(kPage - 1);
    uint64_t end = (reinterpret_cast<uint64_t>(to) + kPage - 1) & ~(kPage - 1);
    for (uint64_t v = start; v < end; v += kPage) {
        uint64_t* pte = entry(v, 0, true);
        *pte = (g_kernel_phys + (v - kKernelVirtualBase)) | leaf_flags(flags, false);
    }
}

}  // namespace

void init(uint64_t hhdm, uint64_t kernel_phys, const MemoryRange* ranges, size_t count) {
    g_hhdm = hhdm;
    g_kernel_phys = kernel_phys;
    enable_cpu_features();

    g_pml4 = new_table();
    for (size_t i = 0; i < count; ++i) map_range_hhdm(ranges[i].base, ranges[i].length, ranges[i].framebuffer);

    map_kernel_section(__kernel_start, __text_start, PageFlags::Write);  // Limine requests (written by the bootloader)
    map_kernel_section(__text_start, __text_end, PageFlags::Execute);
    map_kernel_section(__rodata_start, __rodata_end, PageFlags::None);
    map_kernel_section(__data_start, __data_end, PageFlags::Write);
    map_kernel_section(__symbols_start, __symbols_end, PageFlags::None);

    __asm__ volatile("mov %0, %%cr3" : : "r"(pmm::virt_to_phys(g_pml4)) : "memory");
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0 | (1ull << 16)) : "memory");  // WP: ring 0 honours read-only pages
    g_ready = true;
}

bool ready() { return g_ready; }

uint64_t root_phys() { return pmm::virt_to_phys(g_pml4); }

void load_on_this_cpu() {
    enable_cpu_features();  // NX and PAT are per-CPU MSRs
    __asm__ volatile("mov %0, %%cr3" : : "r"(root_phys()) : "memory");
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0 | (1ull << 16)) : "memory");
}

bool map(uint64_t virt, uint64_t phys, PageFlags flags) {
    sync::ScopedLock vm_guard(g_vm_lock);
    uint64_t* pte = entry(virt & ~(kPage - 1), 0, true);
    if (!pte || (*pte & kPresent)) return false;
    *pte = (phys & kAddress) | leaf_flags(flags, false);
    invalidate(virt);
    return true;
}

bool unmap(uint64_t virt) {
    sync::ScopedLock vm_guard(g_vm_lock);
    uint64_t* pte = entry(virt, 0, false);
    if (!pte || !(*pte & kPresent)) return false;
    *pte = 0;
    invalidate(virt);
    smp::tlb_shootdown(virt);
    return true;
}

bool protect(uint64_t virt, PageFlags flags) {
    sync::ScopedLock vm_guard(g_vm_lock);
    uint64_t* pte = entry(virt, 0, false);
    if (!pte || !(*pte & kPresent)) return false;
    *pte = (*pte & kAddress) | leaf_flags(flags, false);
    invalidate(virt);
    smp::tlb_shootdown(virt);
    return true;
}

uint64_t translate(uint64_t virt) {
    uint64_t* t = root();
    for (int l = 3; l >= 0; --l) {
        uint64_t e = t[(virt >> (12 + 9 * l)) & 0x1FF];
        if (!(e & kPresent)) return ~0ull;
        if (l == 0) return (e & kAddress) | (virt & 0xFFF);
        if ((e & kHuge) && (l == 1 || l == 2)) {
            uint64_t size = 1ull << (12 + 9 * l);
            return (e & kAddress & ~(size - 1)) | (virt & (size - 1));
        }
        t = table(e);
    }
    return ~0ull;
}

bool map_fresh(uint64_t virt, size_t size, PageFlags flags) {
    sync::ScopedLock vm_guard(g_vm_lock);
    uint64_t pages = (size + kPage - 1) / kPage;
    for (uint64_t i = 0; i < pages; ++i) {
        void* frame = pmm::alloc_page();
        if (!frame || !map(virt + i * kPage, pmm::virt_to_phys(frame), flags)) {
            if (frame) pmm::free_page(frame);
            for (uint64_t j = 0; j < i; ++j) {  // roll back
                uint64_t v = virt + j * kPage;
                uint64_t phys = translate(v);
                unmap(v);
                pmm::free_page(pmm::phys_to_virt(phys));
            }
            return false;
        }
    }
    return true;
}

void* map_mmio(uint64_t phys, size_t size) {
    sync::ScopedLock vm_guard(g_vm_lock);
    uint64_t first = phys & ~(kPage - 1);
    uint64_t last = (phys + size + kPage - 1) & ~(kPage - 1);
    for (uint64_t p = first; p < last; p += kPage) {
        uint64_t* pte = entry(g_hhdm + p, 0, true);
        if (!pte || (*pte & kPresent)) continue;  // already reachable (RAM in the HHDM, or mapped earlier)
        *pte = p | leaf_flags(PageFlags::Write | PageFlags::Uncached, false);
        invalidate(g_hhdm + p);
    }
    return reinterpret_cast<void*>(g_hhdm + phys);
}

bool grow_heap(size_t min_bytes) {
    sync::ScopedLock vm_guard(g_vm_lock);
    uint64_t chunk = (min_bytes > kHeapChunk ? min_bytes : kHeapChunk);
    chunk = (chunk + kPage - 1) & ~(kPage - 1);
    if (g_heap_end + chunk > kHeapBase + kHeapLimit) return false;
    if (!map_fresh(g_heap_end, chunk, PageFlags::Write)) return false;
    heap::add_region(reinterpret_cast<void*>(g_heap_end), chunk);
    g_heap_end += chunk;
    return true;
}

uint64_t create_stack(size_t size) {
    sync::ScopedLock vm_guard(g_vm_lock);
    size = (size + kPage - 1) & ~(kPage - 1);
    if (g_stack_count == 64) return 0;
    uint64_t guard = g_stack_next;
    uint64_t bottom = guard + kPage;  // the guard page itself is never mapped
    if (!map_fresh(bottom, size, PageFlags::Write)) return 0;
    g_stack_guards[g_stack_count++] = guard;
    g_stack_next = bottom + size;
    return bottom + size;
}

bool is_stack_guard(uint64_t address) {
    for (uint32_t i = 0; i < g_stack_count; ++i)
        if (address >= g_stack_guards[i] && address < g_stack_guards[i] + kPage) return true;
    return false;
}

Stats stats() {
    Stats s{};
    s.table_pages = g_table_pages;
    s.heap_mapped = g_heap_end - kHeapBase;
    s.stacks = g_stack_count;
    s.kernel_phys = g_kernel_phys;
    s.text_bytes = uint64_t(__text_end - __text_start);
    s.rodata_bytes = uint64_t(__rodata_end - __rodata_start);
    s.data_bytes = uint64_t(__data_end - __data_start);
    s.nx = g_nx;
    s.write_combining = g_wc;
    return s;
}

}  // namespace neko::vmm
