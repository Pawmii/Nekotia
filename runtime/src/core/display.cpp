// Display registry, /dev/fbN devices and the bootfb driver (see neko/display.hpp).
#include <neko/display.hpp>
#include <neko/fmt.hpp>
#include <neko/process.hpp>
#include <neko/runtime.hpp>
#include <neko/vfs.hpp>

namespace neko::display {
namespace {

using namespace proc;  // error codes

constexpr uint64_t kPage = pmm::kPageSize;

struct Slot {
    Device dev;
    bool used;
    uint32_t owner;    // pid of the process that opened it for writing (0 = none; kernel threads: ~0)
    uint64_t size;     // bytes, page aligned
    char path[12];
};

sync::SpinLock g_lock;
Slot g_slots[kMaxDisplays];
uint32_t g_count = 0;

// A log line built from pieces.
struct Line {
    char text[128];
    uint32_t n = 0;
    Line& operator<<(const char* s) {
        while (*s && n < sizeof(text) - 1) text[n++] = *s++;
        text[n] = 0;
        return *this;
    }
    Line& operator<<(uint64_t v) {
        char digits[24];
        fmt::udec(digits, v);
        return *this << digits;
    }
};

uint32_t caller() {
    uint32_t pid = proc::current_pid();
    return pid ? pid : ~0u;
}

int64_t fb_open(void* context, uint32_t flags) {
    auto* s = static_cast<Slot*>(context);
    if (!(flags & vfs::kWrite)) return 0;
    {
        sync::ScopedLock guard(g_lock);
        if (s->owner) return kErrBusy;
        s->owner = caller();
    }
    if (s->dev.flags & kPrimary) term::pause_console(true);
    if (s->dev.driver->owner_changed) s->dev.driver->owner_changed(s->dev.context, s->owner);
    log::info((Line() << "display: " << s->path << " taken by pid " << uint64_t(s->owner == ~0u ? 0 : s->owner)).text);
    return 0;
}

void fb_close(void* context, uint32_t flags) {
    auto* s = static_cast<Slot*>(context);
    if (!(flags & vfs::kWrite)) return;
    {
        sync::ScopedLock guard(g_lock);
        s->owner = 0;
    }
    if (s->dev.driver->owner_changed) s->dev.driver->owner_changed(s->dev.context, 0);
    if (s->dev.flags & kPrimary) term::pause_console(false);  // the console repaints the whole screen
    log::info((Line() << "display: " << s->path << " released").text);
}

uint64_t fb_size(void* context) { return static_cast<Slot*>(context)->size; }

int64_t fb_read(void* context, uint64_t offset, void* buffer, uint64_t length) {
    auto* s = static_cast<Slot*>(context);
    uint64_t bytes = uint64_t(s->dev.pitch) * s->dev.height;
    if (offset >= bytes) return 0;
    if (length > bytes - offset) length = bytes - offset;
    mem::copy(buffer, s->dev.virt + offset, length);
    return int64_t(length);
}

int64_t fb_write(void* context, uint64_t offset, const void* buffer, uint64_t length) {
    auto* s = static_cast<Slot*>(context);
    uint64_t bytes = uint64_t(s->dev.pitch) * s->dev.height;
    if (offset >= bytes) return length ? kErrNoMemory : 0;
    if (length > bytes - offset) length = bytes - offset;
    mem::copy(s->dev.virt + offset, buffer, length);
    if (s->dev.driver->present) s->dev.driver->present(s->dev.context, 0, 0, s->dev.width, s->dev.height);
    return int64_t(length);
}

int64_t fb_map(void* context, uint64_t offset, uint64_t& phys, bool& write_combining) {
    auto* s = static_cast<Slot*>(context);
    if (offset >= s->size) return kErrBadArgument;
    phys = s->dev.phys + offset;
    write_combining = true;
    return 0;
}

void fill_info(const Slot& s, Info& out) {
    out = {};
    out.width = s.dev.width;
    out.height = s.dev.height;
    out.pitch = s.dev.pitch;
    out.bpp = s.dev.bpp;
    out.format = s.dev.format;
    out.flags = s.dev.flags;
    out.size = s.size;
    str::copy(reinterpret_cast<char*>(out.driver), s.dev.driver->name, sizeof(out.driver));
    out.refresh_hz = s.dev.refresh_hz;
    out.owner = s.owner == ~0u ? 0 : s.owner;
}

int64_t fb_control(void* context, uint32_t flags, uint64_t request, uint64_t arg) {
    auto* s = static_cast<Slot*>(context);
    switch (request) {
        case kIoctlInfo: {
            Info i;
            {
                sync::ScopedLock guard(g_lock);
                fill_info(*s, i);
            }
            return copy_to_user(reinterpret_cast<void*>(arg), &i, sizeof(i)) ? 0 : kErrBadAddress;
        }
        case kIoctlPresent: {
            if (!(flags & vfs::kWrite)) return kErrAccess;
            Rect r;
            if (!copy_from_user(&r, reinterpret_cast<void*>(arg), sizeof(r))) return kErrBadAddress;
            if (!s->dev.driver->present) return 0;  // the CPU draws straight into the scanout memory
            int64_t x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y;
            int64_t x1 = int64_t(r.x) + r.w, y1 = int64_t(r.y) + r.h;
            if (x1 > s->dev.width) x1 = s->dev.width;
            if (y1 > s->dev.height) y1 = s->dev.height;
            if (x1 > x0 && y1 > y0)
                s->dev.driver->present(s->dev.context, uint32_t(x0), uint32_t(y0), uint32_t(x1 - x0), uint32_t(y1 - y0));
            return 0;
        }
    }
    if (request >= kIoctlDriverBase && s->dev.driver->control) return s->dev.driver->control(s->dev.context, flags, request, arg);
    return kErrNotSupported;
}

const vfs::DeviceOps kFbOps = {fb_read, fb_write, fb_size, fb_close, nullptr, fb_open, nullptr, fb_map, fb_control};

// ---------------------------------------------------------------- bootfb

const Driver kBootFb = {"bootfb", nullptr, nullptr, nullptr};

}  // namespace

int32_t add(const Device& d) {
    Slot* s = nullptr;
    int32_t index;
    {
        sync::ScopedLock guard(g_lock);
        if (g_count == kMaxDisplays) return -1;
        index = int32_t(g_count++);
        s = &g_slots[index];
        s->dev = d;
        if (index == 0) s->dev.flags |= kPrimary;
        s->size = mem::align_up(uint64_t(d.pitch) * d.height, kPage);
        s->used = true;
        str::copy(s->path, "/dev/fb0", sizeof(s->path));
        s->path[7] = char('0' + index);
    }
    vfs::create_device(s->path, &kFbOps, s);
    log::info((Line() << "display: " << s->path << " " << uint64_t(d.width) << "x" << uint64_t(d.height) << ", "
                      << uint64_t(d.bpp) << " bpp (" << d.driver->name << ")").text);
    return index;
}

uint32_t count() { return g_count; }

bool info(uint32_t index, Info& out) {
    sync::ScopedLock guard(g_lock);
    if (index >= g_count) return false;
    fill_info(g_slots[index], out);
    return true;
}

bool owned(uint32_t index) {
    sync::ScopedLock guard(g_lock);
    return index < g_count && g_slots[index].owner != 0;
}

void init(const Framebuffer& fb) {
    if (!fb.Address || fb.Bpp != 32) return;
    Device d{};
    d.driver = &kBootFb;
    d.width = uint32_t(fb.Width);
    d.height = uint32_t(fb.Height);
    d.pitch = uint32_t(fb.Pitch);
    d.bpp = fb.Bpp;
    d.format = kFormatXrgb8888;
    d.virt = reinterpret_cast<uint8_t*>(fb.Address);
    d.phys = pmm::virt_to_phys(fb.Address);
    add(d);
}

}  // namespace neko::display
