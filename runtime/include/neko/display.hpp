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
// The first driver is `bootfb`: the linear framebuffer the bootloader set up (Limine/GOP/VBE). `bochs`
// (drivers/bochs.cpp) takes over display 0 on QEMU's standard VGA and adds a second buffer: the server draws
// into the hidden one and flips, so the screen never shows a half-drawn frame.
//
// Frames: a display may have several buffers (Info::buffers, each Info::buffer_offset bytes after the
// previous one in the mapping); kIoctlFlip shows buffer N. Vertical blank: drivers that know the scanout
// timing report it through `vblank`; the others get a 60 Hz software clock, so a server can always pace
// itself with kIoctlVblank (and kIoctlWaitVblank blocks until the next one).
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
constexpr uint64_t kIoctlFlip = 0x4403;     // arg: buffer index — show it (owner only; displays with buffers > 1)
constexpr uint64_t kIoctlVblank = 0x4404;   // arg: Vblank* — when the next vertical blank comes
constexpr uint64_t kIoctlWaitVblank = 0x4405;  // arg: unused — sleep until the next vertical blank
constexpr uint64_t kIoctlPresentBuffer = 0x4406;  // arg: PresentArgs* — like PRESENT, for a given buffer (double buffering)
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
    uint32_t buffers;     // 1 = draw where it is shown; 2 = double buffering with kIoctlFlip
    uint32_t shown;       // buffer on screen now
    uint64_t buffer_offset;  // bytes from one buffer to the next in the mapping
};

struct Vblank {
    uint64_t sequence;  // vertical blanks since boot
    uint64_t next_ms;   // uptime of the next one
    uint32_t period_us;
    uint32_t hardware;  // 1 when the driver knows the scanout timing, 0 for the software clock
};

struct Rect {
    int32_t x, y, w, h;
};

struct PresentArgs {
    uint32_t buffer;
    Rect rect;
};

// What a driver implements. Every callback is optional.
struct Driver {
    const char* name;
    // Copies [x, x+w) × [y, y+h) of buffer `buffer` of the CPU-visible memory to the device (already clipped);
    // for the buffer on screen that makes it visible, for a hidden one the next flip shows it.
    void (*present)(void* context, uint32_t buffer, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
    // Driver-specific ioctl (request ≥ kIoctlDriverBase); `flags` are the file's open flags.
    int64_t (*control)(void* context, uint32_t flags, uint64_t request, uint64_t arg);
    // The owner changed (pid, 0 = released): a driver may reset its engine state.
    void (*owner_changed)(void* context, uint32_t pid);
    // Shows buffer `index` (0 … buffers-1). Must be callable with interrupts off (the stop screen uses it).
    void (*flip)(void* context, uint32_t index);
    // Fills `out` from the hardware's scanout timing; false = use the software clock.
    bool (*vblank)(void* context, Vblank& out);
    // Memory that is not physically contiguous (pages of guest RAM): the frame behind byte `offset` of the
    // mapping, and whether to map it write-combining. Without it, mmap uses Device::phys + offset.
    uint64_t (*page)(void* context, uint64_t offset, bool& write_combining);
};

struct Device {
    const Driver* driver;
    void* context;
    uint32_t width, height, pitch, bpp, format;
    uint32_t flags;      // kNeedsPresent / kAccelerated (kPrimary is set by add for display 0)
    uint32_t refresh_hz;
    uint32_t buffers;    // 0 or 1: one; 2: double buffering (the driver has `flip`)
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

// A better driver for a display that is already registered (same memory, e.g. bochs over bootfb). Only while
// nobody owns it. Returns false when the index is wrong or the display is taken.
bool replace(uint32_t index, const Device& d);
const Device* get(uint32_t index);  // nullptr past the end (the description; do not keep it across replace)

// The stop screen: show buffer 0 of the primary display again, whatever a server left on screen; and once
// it is drawn, push it to devices that need PRESENT.
void emergency_reset();
void emergency_present();

}  // namespace neko::display
