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

#include <neko/runtime.hpp>

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

int main(int argc, char** argv) {
    void* heap = aligned_alloc(4096, kHeapSize);
    void* pages = aligned_alloc(4096, kPagesSize);
    auto* screen = static_cast<uint32_t*>(calloc(kScreenWidth * kScreenHeight, sizeof(uint32_t)));
    if (!heap || !pages || !screen) {
        fprintf(stderr, "[neko-host] cannot allocate arenas\n");
        return 1;
    }

    neko::heap::add_region(heap, kHeapSize);
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

    printf("[neko-host] running the kernel as a Linux process\n");
    neko_kernel_main(&g_info);
    printf("[neko-host] kernel entry returned\n");
    finish(0);
}
