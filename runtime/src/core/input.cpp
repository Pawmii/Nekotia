// The input event queue and /dev/input (see neko/input.hpp).
#include <neko/display.hpp>
#include <neko/input.hpp>
#include <neko/process.hpp>
#include <neko/runtime.hpp>
#include <neko/vfs.hpp>

namespace neko::input {
namespace {

using namespace proc;  // error codes

constexpr uint32_t kQueue = 1024;

sync::SpinLock g_lock;  // the queue and the grab
Event g_queue[kQueue];
uint32_t g_head = 0, g_count = 0;
uint32_t g_waiting = 0;
sync::Semaphore g_readable;
volatile uint32_t g_grab = 0;  // pid of the reader (~0 = a kernel thread), 0 = nobody
uint64_t g_events = 0, g_dropped = 0;

void push(const Event& e) {
    if (!g_grab) return;
    {
        sync::ScopedLock guard(g_lock);
        if (!g_grab) return;
        if (g_count == kQueue) {
            ++g_dropped;
            return;
        }
        // Coalesce: consecutive pointer moves become one event (a slow reader sees fewer, bigger steps).
        if (g_count && (e.type == kMotion || e.type == kAbsolute)) {
            Event& last = g_queue[(g_head + g_count - 1) % kQueue];
            if (last.type == e.type && last.mods == e.mods) {
                if (e.type == kMotion) last.x += e.x, last.y += e.y;
                else last.x = e.x, last.y = e.y;
                last.time_ms = e.time_ms;
                return;
            }
        }
        g_queue[(g_head + g_count) % kQueue] = e;
        ++g_count;
        ++g_events;
        while (g_waiting) {
            --g_waiting;
            g_readable.signal();
        }
    }
    vfs::notify();
}

Event make(uint16_t type) {
    Event e{};
    e.time_ms = time::uptime_ms();
    e.type = type;
    return e;
}

int64_t input_open(void*, uint32_t flags) {
    if (flags & vfs::kWrite) return kErrAccess;
    uint32_t pid = proc::current_pid();
    sync::ScopedLock guard(g_lock);
    if (g_grab) return kErrBusy;
    g_grab = pid ? pid : ~0u;
    g_head = g_count = 0;
    return 0;
}

void input_close(void*, uint32_t) {
    {
        sync::ScopedLock guard(g_lock);
        g_grab = 0;
        g_count = 0;
        while (g_waiting) {
            --g_waiting;
            g_readable.signal();
        }
    }
    log::info("input: released to the console");
}

int64_t input_read(void*, uint64_t, void* buffer, uint64_t length) {
    uint64_t max = length / sizeof(Event);
    if (max == 0) return kErrBadArgument;
    auto* out = static_cast<Event*>(buffer);
    for (;;) {
        g_lock.lock();
        if (g_count) {
            uint64_t n = g_count < max ? g_count : max;
            for (uint64_t i = 0; i < n; ++i) out[i] = g_queue[(g_head + i) % kQueue];
            g_head = uint32_t((g_head + n) % kQueue);
            g_count -= uint32_t(n);
            g_lock.unlock();
            return int64_t(n * sizeof(Event));
        }
        ++g_waiting;
        g_lock.unlock();
        if (!g_readable.wait_interruptible()) return kErrInterrupted;
    }
}

uint32_t input_poll(void*, uint32_t) {
    sync::ScopedLock guard(g_lock);
    return g_count ? vfs::kPollIn : 0;
}

const vfs::DeviceOps kInputOps = {input_read, nullptr, nullptr, input_close, nullptr, input_open, input_poll, nullptr, nullptr};

}  // namespace

void init() { vfs::create_device("/dev/input", &kInputOps, nullptr); }

bool grabbed() { return g_grab != 0; }

void key(uint16_t code, bool pressed, uint8_t mods, uint32_t codepoint) {
    Event e = make(kKey);
    e.code = code;
    e.pressed = pressed;
    e.mods = mods;
    e.codepoint = codepoint;
    push(e);
}

void motion(int32_t dx, int32_t dy) {
    if (!dx && !dy) return;
    Event e = make(kMotion);
    e.x = dx;
    e.y = dy;
    push(e);
}

void buttons(uint8_t& last, uint8_t mask) {
    uint8_t changed = uint8_t(mask ^ last);
    last = mask;
    for (uint16_t b = 0; b < 3; ++b) {
        if (!(changed & (1u << b))) continue;
        Event e = make(kButton);
        e.code = b;
        e.pressed = (mask >> b) & 1;
        push(e);
    }
}

void wheel(int32_t notches) {
    if (!notches) return;
    Event e = make(kWheel);
    e.y = notches;
    push(e);
}

void absolute(int32_t x, int32_t y) {
    Event e = make(kAbsolute);
    e.x = x < 0 ? 0 : x > 32767 ? 32767 : x;
    e.y = y < 0 ? 0 : y > 32767 ? 32767 : y;
    push(e);
}

void zap() {
    uint32_t grabber = g_grab;
    display::Info screen{};
    uint32_t owner = display::info(0, screen) ? screen.owner : 0;
    if (grabber && grabber != ~0u) proc::kill(grabber);
    if (owner && owner != grabber) proc::kill(owner);
}

Stats stats() { return {g_events, g_dropped, g_grab == ~0u ? 0 : g_grab}; }

}  // namespace neko::input
