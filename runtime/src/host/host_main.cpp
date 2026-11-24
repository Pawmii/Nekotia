// Host platform: runs the transpiled kernel as an ordinary Linux process.
//   * serial → stdout
//   * the heap and page allocator get malloc'd arenas (HHDM offset = 0)
//   * the framebuffer is RAM; it is saved as a PPM image when the kernel halts
//   * Cpu.Halt() ends the process; Debug.Panic exits with status 1
// Fast edit-run loop and works with AddressSanitizer/UBSan; it is not a real machine
// (no port I/O, no interrupts, inline asm is compiled out).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <neko/runtime.hpp>
#include <neko/process.hpp>
#include <neko/sched.hpp>
#include <neko/vfs.hpp>

namespace {

constexpr size_t kHeapSize = 16u * 1024 * 1024;
constexpr size_t kPagesSize = 8u * 1024 * 1024;
constexpr uint64_t kScreenWidth = 800;
constexpr uint64_t kScreenHeight = 600;

neko::BootInfo g_info;

void save_framebuffer() {
    const auto& fb = g_info.Screen;
    if (!fb.Address) return;
    const char* path = getenv("NEKO_HOST_FB");
    if (!path) path = "build/host/framebuffer.ppm";
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%lu %lu\n255\n", (unsigned long)fb.Width, (unsigned long)fb.Height);
    for (uint64_t y = 0; y < fb.Height; ++y) {
        auto* row = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(fb.Address) + y * fb.Pitch);
        for (uint64_t x = 0; x < fb.Width; ++x) {
            uint8_t rgb[3] = {uint8_t(row[x] >> 16), uint8_t(row[x] >> 8), uint8_t(row[x])};
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    fprintf(stderr, "[neko-host] framebuffer saved to %s\n", path);
}

NEKO_NORETURN void finish(int code) {
    fflush(stdout);
    if (!neko::heap::check()) {
        fprintf(stderr, "[neko-host] heap check FAILED at exit\n");
        code = code ? code : 2;
    }
    fprintf(stderr, "[neko-host] heap at exit: %zu used, %zu blocks\n", neko::heap::used_bytes(), neko::heap::block_count());
    save_framebuffer();
    exit(code);
}

}  // namespace

namespace neko::serial {
void init() {}
void write_char(uint8_t c) { fputc(c, stdout); }
}  // namespace neko::serial

namespace neko::cpu {
void halt() { finish(0); }
void halt_forever() { finish(0); }
}  // namespace neko::cpu

namespace neko::platform {
void stop_after_panic() { finish(1); }
}  // namespace neko::platform

namespace {
uint64_t monotonic_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000 + uint64_t(ts.tv_nsec) / 1000000;
}
uint64_t g_start_ms = monotonic_ms();
}  // namespace

namespace neko::time {
uint64_t uptime_ms() { return monotonic_ms() - g_start_ms; }
uint32_t timer_hz() { return 1000; }
void sleep_ms(uint64_t ms) {
    timespec ts = {time_t(ms / 1000), long(ms % 1000) * 1000000};
    nanosleep(&ts, nullptr);
}
}  // namespace neko::time

namespace neko::irq {
// No hardware interrupts in host mode: handlers are accepted and never called.
bool register_handler(uint8_t line, Handler) { return line < 16; }
uint64_t count(uint8_t) { return 0; }
uint8_t allocate_msi(MsiHandler, void*, const char*) { return 0; }
uint64_t msi_address() { return 0; }
uint32_t msi_data(uint8_t) { return 0; }
bool msi_info(uint32_t, uint8_t&, const char*&, uint64_t&) { return false; }
}  // namespace neko::irq

namespace neko::power {
void reboot() {
    printf("[neko-host] reboot requested\n");
    finish(0);
}
void test_exit(uint8_t code) {
    printf("[neko-host] test exit %u\n", code);
    finish(code);
}
void shutdown() {
    printf("[neko-host] shutdown requested\n");
    finish(0);
}
}  // namespace neko::power

namespace neko::vmm {
// No page tables in host mode: the "virtual memory" API reports itself unavailable.
bool ready() { return false; }
bool map(uint64_t, uint64_t, PageFlags) { return false; }
bool unmap(uint64_t) { return false; }
bool protect(uint64_t, PageFlags) { return false; }
uint64_t translate(uint64_t virt) { return virt; }
bool map_fresh(uint64_t, size_t, PageFlags) { return false; }
void* map_mmio(uint64_t phys, size_t) { return reinterpret_cast<void*>(phys); }
uint64_t create_stack(size_t) { return 0; }
bool is_stack_guard(uint64_t) { return false; }
Stats stats() { return Stats{}; }
uint64_t* space_create() { return nullptr; }
void space_destroy(uint64_t*) {}
bool space_map(uint64_t*, uint64_t, uint64_t, PageFlags) { return false; }
void space_add_flags(uint64_t*, uint64_t, PageFlags) {}
uint64_t space_translate(uint64_t*, uint64_t) { return ~0ull; }
uint64_t space_unmap(uint64_t*, uint64_t) { return ~0ull; }
uint64_t space_unmap_range(uint64_t*, uint64_t, uint64_t, void (*)(uint64_t, void*), void*) { return 0; }
bool space_protect(uint64_t*, uint64_t, PageFlags) { return false; }
bool space_check(uint64_t*, uint64_t, uint64_t, bool) { return false; }
}  // namespace neko::vmm

// No ring 3 in host mode: spawning a program reports "no user mode".
namespace neko::pci {  // no devices in host mode
void add_ecam(uint64_t, uint16_t, uint8_t, uint8_t) {}
void init() {}
bool uses_ecam() { return false; }
uint32_t count() { return 0; }
bool info(uint32_t, Device&) { return false; }
const Device* get(uint32_t) { return nullptr; }
int32_t find(uint16_t, uint16_t, uint32_t) { return -1; }
int32_t find_class(uint8_t, uint8_t, uint32_t) { return -1; }
uint32_t read32(uint32_t, uint16_t) { return 0xFFFFFFFF; }
uint16_t read16(uint32_t, uint16_t) { return 0xFFFF; }
uint8_t read8(uint32_t, uint16_t) { return 0xFF; }
void write32(uint32_t, uint16_t, uint32_t) {}
void write16(uint32_t, uint16_t, uint16_t) {}
void enable(uint32_t, bool, bool, bool) {}
uint8_t find_capability(uint32_t, uint8_t, uint8_t) { return 0; }
void* map_bar(uint32_t, uint32_t) { return nullptr; }
uint32_t msix_enable(uint32_t) { return 0; }
bool msix_route(uint32_t, uint32_t, uint8_t) { return false; }
bool msi_enable(uint32_t, uint8_t) { return false; }
}  // namespace neko::pci

namespace neko::usb {  // no USB in host mode
uint32_t count() { return 0; }
bool info(uint32_t, Info&) { return false; }
void set_keyboard_sink(ScancodeSink) {}
Mouse mouse() { return Mouse{}; }
Stats stats() { return Stats{}; }
}  // namespace neko::usb

namespace neko::arch {
void enter_user(uint64_t, uint64_t, uint64_t, uint64_t) { neko::panic("user mode is not available in host mode"); }
bool user_mode_available() { return false; }
void enable_smep_smap() {}
bool smep_enabled() { return false; }
bool smap_enabled() { return false; }
}  // namespace neko::arch

namespace {
// Heap growth in host mode: extra malloc'd arenas (not adjacent, which the heap handles).
bool grow_heap(size_t min_bytes) {
    size_t size = min_bytes < (4u << 20) ? (4u << 20) : (min_bytes + 4095) & ~size_t(4095);
    void* arena = aligned_alloc(4096, size);
    if (!arena) return false;
    neko::heap::add_region(arena, size);
    return true;
}
}  // namespace

// ---- scheduler hooks: no timer interrupt, so idling simulates one
namespace {
void* g_host_stacks[256];  // keeps the allocations reachable for LeakSanitizer
size_t g_host_stack_count = 0;
}  // namespace

uint64_t neko_sched_new_stack(size_t size) {
    auto* p = static_cast<uint8_t*>(aligned_alloc(4096, size));
    if (!p) return 0;
    if (g_host_stack_count < 256) g_host_stacks[g_host_stack_count++] = p;
    return reinterpret_cast<uint64_t>(p + size);
}

void neko_sched_switch_to(neko::sched::Thread*) {}
extern "C" uint64_t neko_user_copy(void*, const void*, uint64_t) { return ~0ull; }  // no user memory in host mode

void neko_sched_idle() {
    if (!neko::sched::has_sleepers()) {
        printf("\n[neko-host] every thread is waiting for input that cannot come (no keyboard in host mode)\n");
        finish(0);
    }
    timespec ts = {0, 1000000};
    nanosleep(&ts, nullptr);
    neko::sched::tick();
    neko::sched::preempt_if_needed();
}

namespace neko::smp {
Cpu g_cpus[kMaxCpus];
bool g_percpu_ready = true;
uint32_t online_count() { return 1; }
uint32_t cpu_count() { return 1; }
void tlb_shootdown(uint64_t) {}
void tlb_shootdown_range(uint64_t, uint64_t) {}
void tlb_flush_local(uint64_t, uint64_t) {}
volatile uint64_t g_tlb_seq = 0;
void tlb_service_slow() {}
void halt_others() {}
void kick(uint32_t) {}
}  // namespace neko::smp

namespace neko::sysinfo {
const char* cpu_brand() { return "host CPU (Linux process)"; }
uint32_t cpu_count() { return 1; }
uint64_t cpu_mhz() { return 0; }
bool apic_available() { return false; }
}  // namespace neko::sysinfo

int main(int argc, char** argv) {
    void* heap = aligned_alloc(4096, kHeapSize);
    void* pages = aligned_alloc(4096, kPagesSize);
    auto* screen = static_cast<uint32_t*>(calloc(kScreenWidth * kScreenHeight, sizeof(uint32_t)));
    if (!heap || !pages || !screen) {
        fprintf(stderr, "[neko-host] cannot allocate arenas\n");
        return 1;
    }

    neko::heap::add_region(heap, kHeapSize);
    neko::heap::set_grower(grow_heap);
    neko::pmm::init(0);
    neko::pmm::add_region(pages, kPagesSize);

    g_info.HhdmOffset = 0;
    g_info.TotalMemory = kHeapSize + kPagesSize;
    g_info.UsableMemory = kHeapSize + kPagesSize;
    g_info.MemoryMapEntries = 2;
    g_info.Screen = {screen, kScreenWidth, kScreenHeight, kScreenWidth * 4, 32};
    g_info.BootloaderName = reinterpret_cast<const uint8_t*>("nekotia-host");
    g_info.BootloaderVersion = reinterpret_cast<const uint8_t*>("linux");
    g_info.CommandLine = reinterpret_cast<const uint8_t*>(argc > 1 ? argv[1] : "");
    g_info.Hosted = true;

    neko::smp::g_cpus[0].self = &neko::smp::g_cpus[0];
    neko::smp::g_cpus[0].online = true;
    neko::sched::init(0, 0);
    neko::console::init(g_info.Screen);
    neko::vfs::init();
    neko::input::init();
    neko::term::write("[neko-host] running the kernel as a Linux process\n");
    neko_kernel_init();
    neko_kernel_main(&g_info);
    printf("[neko-host] kernel entry returned\n");
    finish(0);
}
