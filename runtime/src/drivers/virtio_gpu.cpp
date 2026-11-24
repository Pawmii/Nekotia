// virtio-gpu (2D): QEMU's `-vga virtio` / `-device virtio-gpu-pci`. The first display driver that works like a
// real GPU with memory of its own: the CPU draws into guest pages ("backing"), and nothing reaches the screen
// until it is copied to the host resource (TRANSFER_TO_HOST_2D) and flushed (RESOURCE_FLUSH) — so the display
// has kNeedsPresent and a server must PRESENT what it changed.
//
//   control queue: GET_DISPLAY_INFO → two resources (B8G8R8X8, the bootloader's size) with scattered pages
//   as backing → SET_SCANOUT to resource 1. Double buffering: buffer N is resource N+1, a flip is SET_SCANOUT
//   plus a flush. The pages are mapped back to back in kernel space (the console draws there) and handed to
//   mmap page by page (Driver::page).
//
// Commands are sent one at a time and their completion is polled (they take microseconds): that also works
// on the stop screen with interrupts off.
#include <neko/display.hpp>
#include <neko/runtime.hpp>

#include "virtio.hpp"

namespace neko::drivers {
namespace {

constexpr uint32_t kGetDisplayInfo = 0x0100, kResourceCreate2d = 0x0101, kSetScanout = 0x0103, kResourceFlush = 0x0104,
                   kTransferToHost2d = 0x0105, kAttachBacking = 0x0106;
constexpr uint32_t kOkNoData = 0x1100, kOkDisplayInfo = 0x1101;
constexpr uint32_t kFormatB8G8R8X8 = 2;
constexpr uint64_t kBufferBase = 0xffffd80000000000ull;  // kernel address of buffer 0 (buffer 1 follows)

struct Header {
    uint32_t type, flags;
    uint64_t fence;
    uint32_t context;
    uint8_t ring, padding[3];
};

struct GpuRect {
    uint32_t x, y, w, h;
};

struct Gpu {
    virtio::Device dev;
    virtio::Queue control;
    uint8_t* request;   // a page: the command
    uint8_t* response;  // a page: the answer
    uint32_t width, height, pitch;
    uint64_t pages;     // per buffer
    uint64_t* frames;   // physical address of every page of both buffers
    uint32_t shown;
    sync::SpinLock lock;
    bool ready;
};

Gpu g_gpu;

// Sends the request (`length` bytes at g.request) and waits for the answer; its type.
uint32_t command(Gpu& g, uint32_t length, uint32_t answer = sizeof(Header)) {
    g.control.desc[0] = {pmm::virt_to_phys(g.request), length, virtio::kDescNext, 1};
    g.control.desc[1] = {pmm::virt_to_phys(g.response), answer, virtio::kDescWrite, 0};
    mem::set(g.response, 0, answer);
    virtio::publish(g.control, 0);
    uint32_t head, got;
    for (uint64_t i = 0; i < 100000000; ++i) {
        if (virtio::next_used(g.control, head, got)) return reinterpret_cast<Header*>(g.response)->type;
        cpu::pause();
    }
    return 0;
}

Header* begin(Gpu& g, uint32_t type) {
    mem::set(g.request, 0, 4096);
    auto* h = reinterpret_cast<Header*>(g.request);
    h->type = type;
    return h;
}

bool transfer(Gpu& g, uint32_t resource, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    begin(g, kTransferToHost2d);
    auto* r = reinterpret_cast<GpuRect*>(g.request + sizeof(Header));
    *r = {x, y, w, h};
    uint64_t offset = uint64_t(y) * g.pitch + uint64_t(x) * 4;
    mem::copy(g.request + sizeof(Header) + 16, &offset, 8);
    mem::copy(g.request + sizeof(Header) + 24, &resource, 4);
    return command(g, sizeof(Header) + 32) == kOkNoData;
}

bool flush(Gpu& g, uint32_t resource, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    begin(g, kResourceFlush);
    auto* r = reinterpret_cast<GpuRect*>(g.request + sizeof(Header));
    *r = {x, y, w, h};
    mem::copy(g.request + sizeof(Header) + 16, &resource, 4);
    return command(g, sizeof(Header) + 24) == kOkNoData;
}

bool scanout(Gpu& g, uint32_t resource) {
    begin(g, kSetScanout);
    auto* r = reinterpret_cast<GpuRect*>(g.request + sizeof(Header));
    *r = {0, 0, g.width, g.height};
    uint32_t ids[2] = {0, resource};
    mem::copy(g.request + sizeof(Header) + 16, ids, 8);
    return command(g, sizeof(Header) + 24) == kOkNoData;
}

// ---- display::Driver

void present(void*, uint32_t buffer, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    Gpu& g = g_gpu;
    sync::ScopedLock guard(g.lock);
    transfer(g, buffer + 1, x, y, w, h);
    if (buffer == g.shown) flush(g, buffer + 1, x, y, w, h);
}

void flip(void*, uint32_t index) {
    Gpu& g = g_gpu;
    sync::ScopedLock guard(g.lock);
    g.shown = index;
    scanout(g, index + 1);
    flush(g, index + 1, 0, 0, g.width, g.height);
}

uint64_t page(void*, uint64_t offset, bool& write_combining) {
    Gpu& g = g_gpu;
    write_combining = false;  // ordinary RAM, also mapped write-back in the kernel: the same memory type
    uint64_t i = offset / 4096;
    return i < 2 * g.pages ? g.frames[i] : ~0ull;
}

const display::Driver kDriver = {"virtio-gpu", present, nullptr, nullptr, flip, nullptr, page};

}  // namespace

void probe_virtio_gpu(uint32_t pci) {
    Gpu& g = g_gpu;
    const display::Device* current = display::get(0);
    if (g.ready || !current) return;  // it takes over the console's display (the bootloader's size)
    if (!virtio::attach(g.dev, pci)) return virtio::fail(g.dev, "virtio-gpu", "no virtio 1.0 capabilities");
    if (!virtio::negotiate(g.dev, 0, 0)) return virtio::fail(g.dev, "virtio-gpu", "features not accepted");
    if (!virtio::setup_queue(g.dev, g.control, 0, 64, 0, 0)) return virtio::fail(g.dev, "virtio-gpu", "no control queue");
    g.request = static_cast<uint8_t*>(pmm::alloc_page());
    g.response = static_cast<uint8_t*>(pmm::alloc_page());
    if (!g.request || !g.response) return virtio::fail(g.dev, "virtio-gpu", "out of memory");
    virtio::driver_ok(g.dev);

    begin(g, kGetDisplayInfo);
    if (command(g, sizeof(Header), sizeof(Header) + 16 * 24) != kOkDisplayInfo) {
        log::warn("virtio-gpu: no display info");
        return;
    }
    // The console keeps its size: the scanout gets the bootloader's mode.
    g.width = current->width;
    g.height = current->height;
    g.pitch = g.width * 4;
    g.pages = mem::align_up(uint64_t(g.pitch) * g.height, 4096) / 4096;
    g.frames = static_cast<uint64_t*>(heap::alloc_zeroed(2 * g.pages * sizeof(uint64_t)));
    if (!g.frames) return;
    for (uint64_t i = 0; i < 2 * g.pages; ++i) {
        void* p = pmm::alloc_page();
        if (!p) return log::warn("virtio-gpu: out of memory for the buffers");
        g.frames[i] = pmm::virt_to_phys(p);
        if (!vmm::map(kBufferBase + i * 4096, g.frames[i], vmm::PageFlags::Write)) return log::warn("virtio-gpu: cannot map the buffers");
    }

    // Two resources, each backed by its half of the pages (the entry list can be long: a page holds 256).
    auto* entries = static_cast<uint8_t*>(pmm::alloc_contiguous((g.pages * 16 + 4095) / 4096 + 1));
    if (!entries) return log::warn("virtio-gpu: out of memory");
    for (uint32_t res = 1; res <= 2; ++res) {
        begin(g, kResourceCreate2d);
        uint32_t create[4] = {res, kFormatB8G8R8X8, g.width, g.height};
        mem::copy(g.request + sizeof(Header), create, 16);
        if (command(g, sizeof(Header) + 16) != kOkNoData) return log::warn("virtio-gpu: RESOURCE_CREATE_2D failed");
        // ATTACH_BACKING: header, resource, count — then the entries in a second descriptor.
        begin(g, kAttachBacking);
        uint32_t attach[2] = {res, uint32_t(g.pages)};
        mem::copy(g.request + sizeof(Header), attach, 8);
        for (uint64_t i = 0; i < g.pages; ++i) {
            uint64_t address = g.frames[(res - 1) * g.pages + i];
            uint32_t length = 4096;
            mem::copy(entries + i * 16, &address, 8);
            mem::copy(entries + i * 16 + 8, &length, 4);
            mem::set(entries + i * 16 + 12, 0, 4);
        }
        g.control.desc[0] = {pmm::virt_to_phys(g.request), uint32_t(sizeof(Header) + 8), virtio::kDescNext, 1};
        g.control.desc[1] = {pmm::virt_to_phys(entries), uint32_t(g.pages * 16), virtio::kDescNext, 2};
        g.control.desc[2] = {pmm::virt_to_phys(g.response), uint32_t(sizeof(Header)), virtio::kDescWrite, 0};
        virtio::publish(g.control, 0);
        uint32_t head, got;
        bool done = false;
        for (uint64_t i = 0; i < 100000000 && !done; ++i) {
            done = virtio::next_used(g.control, head, got);
            cpu::pause();
        }
        if (!done || reinterpret_cast<Header*>(g.response)->type != kOkNoData) return log::warn("virtio-gpu: ATTACH_BACKING failed");
    }
    pmm::free_contiguous(entries, (g.pages * 16 + 4095) / 4096 + 1);

    // The console moves over: same picture in buffer 0, then the scanout switches to it.
    auto* buffer0 = reinterpret_cast<uint8_t*>(kBufferBase);
    for (uint32_t y = 0; y < g.height; ++y)
        mem::copy(buffer0 + uint64_t(y) * g.pitch, current->virt + uint64_t(y) * current->pitch, g.pitch);
    Framebuffer fb{reinterpret_cast<uint32_t*>(buffer0), g.width, g.height, g.pitch, 32};
    console::retarget(fb);
    if (!scanout(g, 1)) return log::warn("virtio-gpu: SET_SCANOUT failed");
    transfer(g, 1, 0, 0, g.width, g.height);
    flush(g, 1, 0, 0, g.width, g.height);
    g.ready = true;

    display::Device d = *current;
    d.driver = &kDriver;
    d.context = &g;
    d.pitch = g.pitch;
    d.buffers = 2;
    d.flags = (d.flags & display::kPrimary) | display::kNeedsPresent;
    d.virt = buffer0;
    d.phys = 0;
    d.refresh_hz = 60;
    display::replace(0, d);
    term::write("[neko] virtio-gpu: ");
    term::write_udec(g.width);
    term::write("x");
    term::write_udec(g.height);
    term::write(", 2 host resources (present + flip)\n");
}

}  // namespace neko::drivers
