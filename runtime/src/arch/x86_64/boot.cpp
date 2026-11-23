// x86_64 entry point. Limine loads the ELF in the higher half, already in long mode
// with paging, a GDT and a 64 KiB stack, then jumps to kmain().
//
// kmain:      serial → GDT/IDT → buddy allocator → own page tables → heap → kernel stack (guarded)
// kmain_main: (on the new stack) console → ACPI/APIC → PCI → initrd → C# static init → SMP → drivers → [Irq] → sti → C# [KernelEntry]
#include <limine.h>

#include "arch.hpp"

#include <neko/block.hpp>
#include <neko/pci.hpp>
#include <neko/process.hpp>
#include <neko/sched.hpp>
#include <neko/smp.hpp>
#include <neko/vfs.hpp>

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

__attribute__((used, section(".limine_requests")))
volatile limine_mp_request mp_request = {.id = LIMINE_MP_REQUEST_ID, .revision = 0, .response = nullptr, .flags = 0};

__attribute__((used, section(".limine_requests")))
volatile limine_module_request module_request = {.id = LIMINE_MODULE_REQUEST_ID, .revision = 0, .response = nullptr,
                                                 .internal_module_count = 0, .internal_modules = nullptr};

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

// Copied out of the bootloader's memory, which is handed to the page allocator later.
char g_bootloader_name[64], g_bootloader_version[64], g_cmdline[256];

void init_strings() {
    auto* info = bootloader_info_request.response;
    neko::str::copy(g_bootloader_name, info ? info->name : "unknown", sizeof(g_bootloader_name));
    neko::str::copy(g_bootloader_version, info ? info->version : "", sizeof(g_bootloader_version));
    auto* cmd = cmdline_request.response;
    neko::str::copy(g_cmdline, cmd ? cmd->cmdline : "", sizeof(g_cmdline));
    g_info.BootloaderName = cstr(g_bootloader_name);
    g_info.BootloaderVersion = cstr(g_bootloader_version);
    g_info.CommandLine = cstr(g_cmdline);
}

// "Bootloader reclaimable" memory (Limine's page tables, stacks, responses) once nothing needs it:
// our own tables, GDT, IDT and stacks are in use, every AP runs on its own stack, the strings are
// copied and the modules (initrd) live in a different region type.
uint64_t reclaim_bootloader_memory() {
    auto* map = memmap_request.response;
    if (!map || neko::smp::online_count() < neko::arch::smp_started()) return 0;
    struct Range { uint64_t base, length; } ranges[64];
    uint32_t n = 0;
    for (uint64_t i = 0; i < map->entry_count && n < 64; ++i) {  // copy first: the map itself is in there
        limine_memmap_entry* e = map->entries[i];
        if (e->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE && e->base != 0) ranges[n++] = {e->base, e->length};
    }
    uint64_t total = 0;
    for (uint32_t i = 0; i < n; ++i) {
        neko::pmm::add_region(reinterpret_cast<void*>(ranges[i].base + g_info.HhdmOffset), ranges[i].length);
        total += ranges[i].length;
    }
    g_info.UsableMemory += total;
    return total;
}

using Constructor = void (*)();
extern "C" Constructor __init_array_start[];
extern "C" Constructor __init_array_end[];

// Modules from limine.conf: the initrd (/boot/initrd.tar) is unpacked into the VFS; any other
// module becomes /bin/NAME.
void init_modules() {
    neko::vfs::init();
    auto* res = module_request.response;
    if (!res) return;
    for (uint64_t i = 0; i < res->module_count; ++i) {
        limine_file* f = res->modules[i];
        neko::vfs::add_module(f->path, f->address, f->size);
    }
}

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

uint64_t g_kernel_stack_top = 0;

[[noreturn]] void kmain_main() {
    neko::sched::init(g_kernel_stack_top, kKernelStackSize);
    init_framebuffer();
    init_strings();
    neko::log::configure(g_cmdline);
    g_info.Hosted = false;

    bool screen = neko::console::init(g_info.Screen);
    neko::term::write("[neko] runtime: x86_64, Limine protocol, ");
    neko::term::write(screen ? "framebuffer console\n" : "serial only (no framebuffer)\n");

    auto vm = neko::vmm::stats();
    neko::term::write("[neko] paging: own tables, NX ");
    neko::term::write(vm.nx ? "on" : "off");
    neko::term::write(", framebuffer ");
    neko::term::write(vm.write_combining ? "write-combining" : "uncached");
    neko::term::write(", kernel stack with guard page");
    neko::term::write(vm.global_pages ? ", global kernel pages\n" : "\n");

    init_interrupts();
    neko::pci::init();
    neko::term::write("[neko] PCI: ");
    neko::term::write_udec(neko::pci::count());
    neko::term::write(neko::pci::uses_ecam() ? " device(s), ECAM (MCFG)\n" : " device(s), legacy port I/O\n");
    init_modules();

    // Dynamic initialisers of generated static fields, then [Irq] handler registration.
    for (Constructor* c = __init_array_start; c != __init_array_end; ++c) (*c)();
    neko_kernel_init();
    neko::cpu::enable_interrupts();

    if (neko::sysinfo::apic_available()) neko::arch::smp_start(mp_request.response);
    neko::term::write("[neko] SMP: ");
    neko::term::write_udec(neko::smp::online_count());
    neko::term::write(" of ");
    neko::term::write_udec(mp_request.response ? mp_request.response->cpu_count : 1);
    neko::term::write(" CPU(s) online\n");
    neko::drivers::init();  // needs interrupts (completion IRQs) and the VFS (/dev nodes)
    uint64_t reclaimed = reclaim_bootloader_memory();
    neko::term::write("[neko] reclaimed ");
    neko::term::write_size(reclaimed);
    neko::term::write(" of bootloader memory\n");
    neko::term::write("[neko] user mode: syscall/sysret, SMEP ");
    neko::term::write(neko::arch::smep_enabled() ? "on" : "off");
    neko::term::write(", SMAP ");
    neko::term::write(neko::arch::smap_enabled() ? "on" : "off");
    neko::term::write("\n");

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

    neko::arch::gdt_init(0);
    neko::arch::percpu_init_bsp();
    neko::arch::idt_init();
    neko::arch::syscall_init_this_cpu();
    init_physical_memory();
    init_virtual_memory();
    neko::arch::enable_smep_smap();

    uint64_t stack = neko::vmm::create_stack(kKernelStackSize);
    if (!stack) neko::panic("cannot allocate the kernel stack");
    g_kernel_stack_top = stack;
    switch_stack(stack, kmain_main);
}
