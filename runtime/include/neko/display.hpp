// Displays (core/display.cpp): the layer every graphics driver plugs into and every display server
// (window manager, compositor) talks to.
//
//   driver  ── display::add(Device) ──►  /dev/fb0, /dev/fb1 …  ◄── open / mmap / ioctl ──  user program
//
// A driver describes its scanout memory (physical address, pitch, format) and, optionally, how to
// push a changed rectangle to the screen (`present`) — needed by GPUs whose scanout is not the memory
// the CPU draws into (virtio-gpu, or a card with its own VRAM and a DMA engine). Drivers with
// acceleration expose it through `control` (their own ioctl requests, from 0x4480 up).
//
// The first driver is `bootfb`: the linear framebuffer the bootloader set up (Limine/GOP/VBE).
//
// Ownership: opening /dev/fbN for writing makes the caller its owner (one at a time; others get
// kErrBusy). While /dev/fb0 has an owner the kernel console stops drawing (it keeps its text and
// repaints everything once the owner closes the device or exits). Reading /dev/fbN gives the raw
// pixels (`cat /dev/fb0 > shot.raw`); read-only opens never conflict.
#pragma once

#include <neko/boot.hpp>
#include <neko/types.hpp>

namespace neko::display {

constexpr uint32_t kMaxDisplays = 4;

// ioctl requests on /dev/fbN.
constexpr uint64_t kIoctlInfo = 0x4401;     // arg: Info* — fills it in
constexpr uint64_t kIoctlPresent = 0x4402;  // arg: Rect* — the rectangle changed (needs the device opened for writing)
constexpr uint64_t kIoctlDriverBase = 0x4480;  // driver-specific requests start here

constexpr uint32_t kFormatXrgb8888 = 1;     // 32 bits: 0x00RRGGBB, little endian (B, G, R, X in memory)

// Info::flags
constexpr uint32_t kNeedsPresent = 1;       // pixels reach the screen only through kIoctlPresent
constexpr uint32_t kAccelerated = 2;        // the driver implements its own ioctl requests (2D/3D engine)
constexpr uint32_t kPrimary = 4;            // the console's screen

// Same layout in sdk/Nekotia.Gui (DisplayInfo).
struct Info {
    uint32_t width, height;
    uint32_t pitch;     // bytes per row
    uint32_t bpp;
    uint32_t format;
    uint32_t flags;
    uint64_t size;      // bytes to mmap (pitch × height, rounded up to pages)
    uint8_t driver[16]; // "bootfb", "virtio-gpu", "nvidia"…
    uint32_t refresh_hz;  // 0 = unknown
    uint32_t owner;       // pid of the owner, 0 = none
};

struct Rect {
    int32_t x, y, w, h;
};

// What a driver implements. Every callback is optional.
struct Driver {
    const char* name;
    // Copies [x, x+w) × [y, y+h) of the CPU-visible memory to the screen (already clipped to the display).
    void (*present)(void* context, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
    // Driver-specific ioctl (request ≥ kIoctlDriverBase); `flags` are the file's open flags.
    int64_t (*control)(void* context, uint32_t flags, uint64_t request, uint64_t arg);
    // The owner changed (pid, 0 = released): a driver may reset its engine state.
    void (*owner_changed)(void* context, uint32_t pid);
};

struct Device {
    const Driver* driver;
    void* context;
    uint32_t width, height, pitch, bpp, format;
    uint32_t flags;      // kNeedsPresent / kAccelerated (kPrimary is set by add for display 0)
    uint32_t refresh_hz;
    uint64_t phys;       // CPU-visible memory (what mmap maps), page aligned
    uint8_t* virt;       // the same memory in the kernel (for read() of the device)
};

// Registers a display and creates /dev/fbN. Returns N, or -1 when the table is full.
int32_t add(const Device& d);
uint32_t count();
bool info(uint32_t index, Info& out);
bool owned(uint32_t index);       // someone draws on it (for the console: display 0)

// Called at boot with the bootloader's framebuffer: registers the bootfb driver as display 0.
void init(const Framebuffer& fb);

}  // namespace neko::display
