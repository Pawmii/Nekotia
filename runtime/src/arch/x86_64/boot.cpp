// x86_64 entry point. Limine loads the ELF in the higher half, already in long mode
// with paging, a GDT and a 64 KiB stack, then jumps to kmain().
//
// kmain:      serial → GDT/IDT → buddy allocator → own page tables → heap → kernel stack (guarded)
// kmain_main: (on the new stack) console → ACPI/APIC → C# static init → [Irq] → sti → C# [KernelEntry]
#include <limine.h>

#include "arch.hpp"

#ifndef NEKO_HEAP_SIZE
#define NEKO_HEAP_SIZE (4u * 1024 * 1024)  // initial heap; it grows on demand
#endif

constexpr size_t kKernelStackSize = 64 * 1024;

namespace {

// ---- Limine requests (must stay in the .limine_requests* sections, see linker.ld)

__attribute__((used, section(".limine_requests_start")))
volatile uint64_t limine_requests_start[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests")))
volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(3);

__attribute__((used, section(".limine_requests")))
volatile limine_framebuffer_request framebuffer_request = {.id = LIMINE_FRAMEBUFFER_REQUEST_ID, .revision = 0, .response = nullptr};

__attribute__((used, section(".limine_requests")))
volatile limine_memmap_request memmap_request = {.id = LIMINE_MEMMAP_REQUEST_ID, .revision = 0, .response = nullptr};

__attribute__((used, section(".limine_requests")))
volatile limine_hhdm_request hhdm_request = {.id = LIMINE_HHDM_REQUEST_ID, .revision = 0, .response = nullptr};

__attribute__((used, section(".limine_requests")))
volatile limine_bootloader_info_request bootloader_info_request = {.id = LIMINE_BOOTLOADER_INFO_REQUEST_ID, .revision = 0, .response = nullptr};

__attribute__((used, section(".limine_requests")))
volatile limine_executable_cmdline_request cmdline_request = {.id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID, .revision = 0, .response = nullptr};

__attribute__((used, section(".limine_requests")))
volatile limine_rsdp_request rsdp_request = {.id = LIMINE_RSDP_REQUEST_ID, .revision = 0, .response = nullptr};

__attribute__((used, section(".limine_requests")))
volatile limine_executable_address_request executable_address_request = {.id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID, .revision = 0, .response = nullptr};

__attribute__((used, section(".limine_requests_end")))
volatile uint64_t limine_requests_end[] = LIMINE_REQUESTS_END_MARKER;

neko::BootInfo g_info;

const uint8_t* cstr(const char* s) { return reinterpret_cast<const uint8_t*>(s ? s : ""); }

neko::vmm::MemoryRange g_ranges[256];
size_t g_range_count = 0;

// Physical memory → buddy allocator; RAM ranges → list for the HHDM of our page tables.
void init_physical_memory() {
    if (!hhdm_request.response || !memmap_request.response) neko::panic("bootloader gave no HHDM or memory map");

    const uint64_t hhdm = hhdm_request.response->offset;
    g_info.HhdmOffset = hhdm;
    neko::pmm::init(hhdm);

    auto* map = memmap_request.response;
    g_info.MemoryMapEntries = map->entry_count;

    for (uint64_t i = 0; i < map->entry_count; ++i) {
        limine_memmap_entry* e = map->entries[i];
        bool ram = false;
        switch (e->type) {  // RAM only; reserved ranges and MMIO holes are not memory
            case LIMINE_MEMMAP_USABLE:
            case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
            case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:
            case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
            case LIMINE_MEMMAP_ACPI_NVS:
                g_info.TotalMemory += e->length;
                ram = true;
                break;
        }
        if ((ram || e->type == LIMINE_MEMMAP_FRAMEBUFFER) && g_range_count < 256)
            g_ranges[g_range_count++] = {e->base, e->length, e->type == LIMINE_MEMMAP_FRAMEBUFFER};
        if (e->type != LIMINE_MEMMAP_USABLE) continue;
        g_info.UsableMemory += e->length;

        uint64_t base = e->base, length = e->length;
        if (base == 0) {  // keep physical page 0 unused so null stays invalid
            if (length <= neko::pmm::kPageSize) continue;
            base += neko::pmm::kPageSize;
            length -= neko::pmm::kPageSize;
        }
        neko::pmm::add_region(reinterpret_cast<void*>(base + hhdm), length);
    }
}

void init_virtual_memory() {
    if (!executable_address_request.response) neko::panic("bootloader did not report the kernel's physical address");
    neko::vmm::init(g_info.HhdmOffset, executable_address_request.response->physical_base, g_ranges, g_range_count);
    neko::heap::set_grower(neko::vmm::grow_heap);
    if (!neko::vmm::grow_heap(NEKO_HEAP_SIZE)) neko::panic("not enough memory for the kernel heap");
}

void init_framebuffer() {
    auto* res = framebuffer_request.response;
    if (!res || res->framebuffer_count == 0) return;
    limine_framebuffer* fb = res->framebuffers[0];
    g_info.Screen.Address = static_cast<uint32_t*>(fb->address);
    g_info.Screen.Width = fb->width;
    g_info.Screen.Height = fb->height;
    g_info.Screen.Pitch = fb->pitch;
    g_info.Screen.Bpp = fb->bpp;
}

void init_strings() {
    auto* info = bootloader_info_request.response;
    g_info.BootloaderName = cstr(info ? info->name : "unknown");
    g_info.BootloaderVersion = cstr(info ? info->version : "");
    auto* cmd = cmdline_request.response;
    g_info.CommandLine = cstr(cmd ? cmd->cmdline : "");
}

using Constructor = void (*)();
extern "C" Constructor __init_array_start[];
extern "C" Constructor __init_array_end[];

void init_interrupts() {
    neko::arch::MadtInfo madt;
    void* rsdp = rsdp_request.response ? rsdp_request.response->address : nullptr;
    if (!neko::arch::acpi_init(rsdp, g_info.HhdmOffset, madt)) {
        neko::term::write("[neko] \x1b[93mwarning:\x1b[0m no ACPI MADT — timer and IRQs are unavailable\n");
        return;
    }
    neko::arch::apic_init(madt);
}

}  // namespace

namespace neko::platform {
void stop_after_panic() { cpu::halt_forever(); }
}  // namespace neko::platform

namespace {

[[noreturn]] void kmain_main();

[[noreturn]] void switch_stack(uint64_t top, void (*next)()) {
    __asm__ volatile(
        "mov %0, %%rsp\n\t"
        "xor %%ebp, %%ebp\n\t"  // end of the frame-pointer chain for stack traces
        "call *%1\n\t"
        "ud2"
        :
        : "r"(top), "r"(next)
        : "memory");
    __builtin_unreachable();
}

[[noreturn]] void kmain_main() {
    init_framebuffer();
    init_strings();
    g_info.Hosted = false;

    bool screen = neko::console::init(g_info.Screen);
    neko::term::write("[neko] runtime: x86_64, Limine protocol, ");
    neko::term::write(screen ? "framebuffer console\n" : "serial only (no framebuffer)\n");

    auto vm = neko::vmm::stats();
    neko::term::write("[neko] paging: own tables, NX ");
    neko::term::write(vm.nx ? "on" : "off");
    neko::term::write(", framebuffer ");
    neko::term::write(vm.write_combining ? "write-combining" : "uncached");
    neko::term::write(", kernel stack with guard page\n");

    init_interrupts();

    // Dynamic initialisers of generated static fields, then [Irq] handler registration.
    for (Constructor* c = __init_array_start; c != __init_array_end; ++c) (*c)();
    neko_kernel_init();
    neko::cpu::enable_interrupts();

    neko::term::write("[neko] heap ");
    neko::term::write_size(neko::heap::total_bytes());
    neko::term::write(" (grows on demand), ");
    neko::term::write_udec(neko::pmm::free_pages());
    neko::term::write(" free pages, ");
    neko::term::write_udec(neko::sysinfo::cpu_count());
    neko::term::write(" CPU(s) @ ");
    neko::term::write_udec(neko::sysinfo::cpu_mhz());
    neko::term::write(" MHz — entering C# kernel\n");

    neko_kernel_main(&g_info);

    neko::term::write("[neko] kernel entry returned, halting\n");
    neko::cpu::halt_forever();
}

}  // namespace

extern "C" NEKO_NORETURN void kmain() {
    neko::serial::init();
    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) neko::panic("bootloader does not support Limine base revision 3");

    neko::arch::gdt_init();
    neko::arch::idt_init();
    init_physical_memory();
    init_virtual_memory();

    uint64_t stack = neko::vmm::create_stack(kKernelStackSize);
    if (!stack) neko::panic("cannot allocate the kernel stack");
    switch_stack(stack, kmain_main);
}
