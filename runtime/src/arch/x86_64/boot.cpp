// x86_64 entry point. Limine loads the ELF in the higher half, already in long mode
// with paging, a GDT and a 64 KiB stack, then jumps to kmain().
#include <limine.h>

#include "arch.hpp"

#ifndef NEKO_HEAP_SIZE
#define NEKO_HEAP_SIZE (16u * 1024 * 1024)
#endif

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

__attribute__((used, section(".limine_requests_end")))
volatile uint64_t limine_requests_end[] = LIMINE_REQUESTS_END_MARKER;

neko::BootInfo g_info;

const uint8_t* cstr(const char* s) { return reinterpret_cast<const uint8_t*>(s ? s : ""); }

void init_memory() {
    if (!hhdm_request.response || !memmap_request.response) neko::panic("bootloader gave no HHDM or memory map");

    const uint64_t hhdm = hhdm_request.response->offset;
    g_info.HhdmOffset = hhdm;
    neko::pmm::init(hhdm);

    auto* map = memmap_request.response;
    g_info.MemoryMapEntries = map->entry_count;

    bool heap_ready = false;
    for (uint64_t i = 0; i < map->entry_count; ++i) {
        limine_memmap_entry* e = map->entries[i];
        switch (e->type) {  // RAM only; reserved ranges and MMIO holes are not memory
            case LIMINE_MEMMAP_USABLE:
            case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
            case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:
            case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
            case LIMINE_MEMMAP_ACPI_NVS:
                g_info.TotalMemory += e->length;
                break;
        }
        if (e->type != LIMINE_MEMMAP_USABLE) continue;
        g_info.UsableMemory += e->length;

        uint64_t base = e->base, length = e->length;
        if (base == 0) {  // keep physical page 0 unused so null stays invalid
            if (length <= neko::pmm::kPageSize) continue;
            base += neko::pmm::kPageSize;
            length -= neko::pmm::kPageSize;
        }
        if (!heap_ready && length >= NEKO_HEAP_SIZE + neko::pmm::kPageSize) {
            neko::heap::add_region(reinterpret_cast<void*>(base + hhdm), NEKO_HEAP_SIZE);
            base += NEKO_HEAP_SIZE;
            length -= NEKO_HEAP_SIZE;
            heap_ready = true;
        }
        neko::pmm::add_region(reinterpret_cast<void*>(base + hhdm), length);
    }
    if (!heap_ready) neko::panic("no usable memory region is large enough for the kernel heap");
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

extern "C" NEKO_NORETURN void kmain() {
    neko::serial::init();
    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) neko::panic("bootloader does not support Limine base revision 3");

    neko::arch::gdt_init();
    neko::arch::idt_init();
    init_memory();
    init_framebuffer();
    init_strings();
    g_info.Hosted = false;

    bool screen = neko::console::init(g_info.Screen);
    neko::term::write("[neko] runtime: x86_64, Limine protocol, ");
    neko::term::write(screen ? "framebuffer console\n" : "serial only (no framebuffer)\n");

    init_interrupts();

    // Dynamic initialisers of generated static fields, then [Irq] handler registration.
    for (Constructor* c = __init_array_start; c != __init_array_end; ++c) (*c)();
    neko_kernel_init();
    neko::cpu::enable_interrupts();

    neko::term::write("[neko] heap ");
    neko::term::write_size(neko::heap::total_bytes());
    neko::term::write(", ");
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
