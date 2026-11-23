// Kernel log ring buffer (see neko/log.hpp).
#include <neko/log.hpp>
#include <neko/runtime.hpp>

namespace neko::log {
namespace {

constexpr uint64_t kSize = 64 * 1024;
uint8_t g_ring[kSize];
uint64_t g_start = 0, g_used = 0;  // oldest byte, bytes stored
sync::SpinLock g_lock;
Level g_level = Info;
volatile bool g_capture = true;
volatile uint32_t g_quiet = 0;  // > 0 while write() prints (its line is already in the ring)

void put(uint8_t c) {  // g_lock held
    if (g_used == kSize) {  // full: drop the oldest byte
        g_start = (g_start + 1) % kSize;
        --g_used;
    }
    g_ring[(g_start + g_used) % kSize] = c;
    ++g_used;
}

void put_str(const char* s) {
    while (s && *s) put(uint8_t(*s++));
}

const char* kNames[] = {"debug", "info ", "warn ", "error"};

}  // namespace

void write(Level level, const char* text) {
    if (level < Debug) level = Debug;
    if (level > Error) level = Error;
    {
        sync::ScopedLock guard(g_lock);
        char stamp[32];
        uint64_t ms = time::uptime_ms();
        int n = 0;
        stamp[n++] = '[';
        char secs[24];
        int len = fmt::udec(secs, ms / 1000);
        for (int i = len; i < 5; ++i) stamp[n++] = ' ';
        for (int i = 0; i < len; ++i) stamp[n++] = secs[i];
        stamp[n++] = '.';
        n += fmt::dec_digits(stamp + n, int64_t(ms % 1000), 3);
        stamp[n++] = ']';
        stamp[n++] = ' ';
        stamp[n] = 0;
        put_str(stamp);
        put_str(kNames[level]);
        put(' ');
        put_str(text);
        put('\n');
    }
    __atomic_add_fetch(&g_quiet, 1, __ATOMIC_ACQ_REL);
    if (level >= g_level) {
        switch (level) {
            case Debug: term::write("\x1b[90m"); break;
            case Info: term::write("\x1b[90m"); break;
            case Warn: term::write("\x1b[93mwarning: "); break;
            default: term::write("\x1b[91merror: "); break;
        }
        term::write(text);
        term::write("\x1b[0m\n");
    } else {  // below the threshold: serial only
        serial::write(kNames[level]);
        serial::write(": ");
        serial::write(text);
        serial::write("\n");
    }
    __atomic_sub_fetch(&g_quiet, 1, __ATOMIC_ACQ_REL);
}

void set_level(Level level) { g_level = level < Debug ? Debug : level > Error ? Error : level; }
Level level() { return g_level; }

void configure(const char* cmdline) {
    for (const char* p = cmdline; p && *p; ++p) {
        if ((p == cmdline || p[-1] == ' ') && str::starts_with(reinterpret_cast<const uint8_t*>(p), "loglevel=")) {
            const uint8_t* v = reinterpret_cast<const uint8_t*>(p + 9);
            if (str::starts_with(v, "debug")) set_level(Debug);
            else if (str::starts_with(v, "info")) set_level(Info);
            else if (str::starts_with(v, "warn")) set_level(Warn);
            else if (str::starts_with(v, "error")) set_level(Error);
        }
    }
}

bool capturing() { return g_capture && __atomic_load_n(&g_quiet, __ATOMIC_ACQUIRE) == 0; }

void capture_byte(uint8_t c) {
    sync::ScopedLock guard(g_lock);
    put(c);
}

void stop_capture() { g_capture = false; }

uint64_t size() {
    sync::ScopedLock guard(g_lock);
    return g_used;
}

int64_t read(uint64_t offset, uint8_t* buffer, uint64_t length) {
    sync::ScopedLock guard(g_lock);
    if (offset >= g_used) return 0;
    uint64_t n = g_used - offset < length ? g_used - offset : length;
    for (uint64_t i = 0; i < n; ++i) buffer[i] = g_ring[(g_start + offset + i) % kSize];
    return int64_t(n);
}

}  // namespace neko::log
