// PC speaker (neko/speaker.hpp). PIT input clock 1.193182 MHz; channel 2, mode 3 (square wave).
#include <neko/platform.hpp>
#include <neko/runtime.hpp>
#include <neko/sched.hpp>
#include <neko/speaker.hpp>

namespace neko::speaker {
namespace {

constexpr uint32_t kPitHz = 1193182;
bool g_enabled = true;

bool has_word(const char* s, const char* word) {
    for (const char* p = s; p && *p; ++p) {
        if (p != s && p[-1] != ' ') continue;
        const char* a = p;
        const char* b = word;
        while (*b && *a == *b) ++a, ++b;
        if (!*b && (*a == 0 || *a == ' ')) return true;
    }
    return false;
}

// A triangle wave in [-1000, 1000] from a phase in [0, 1000).
int32_t triangle(uint32_t phase) {
    phase %= 1000;
    return phase < 500 ? int32_t(phase) * 4 - 1000 : 3000 - int32_t(phase) * 4;
}

}  // namespace

void configure(const char* cmdline) { g_enabled = !has_word(cmdline, "nosound"); }
bool enabled() { return g_enabled; }

void tone(uint32_t hz) {
    if (!g_enabled || hz < 20) return off();
    uint32_t divisor = kPitHz / hz;
    if (divisor > 0xFFFF) divisor = 0xFFFF;
    cpu::outb(0x43, 0xB6);  // channel 2, lobyte/hibyte, mode 3
    cpu::outb(0x42, uint8_t(divisor));
    cpu::outb(0x42, uint8_t(divisor >> 8));
    uint8_t gate = cpu::inb(0x61);
    if ((gate & 3) != 3) cpu::outb(0x61, gate | 3);
}

void off() { cpu::outb(0x61, cpu::inb(0x61) & 0xFC); }

bool wait_us(uint64_t us, bool (*stop)()) {
    uint64_t mhz = sysinfo::cpu_mhz();
    if (mhz == 0) {  // no TSC calibration: port 0x80 writes take about a microsecond
        for (uint64_t i = 0; i < us; ++i) {
            if (stop && (i & 1023) == 0 && stop()) return true;
            cpu::io_wait();
        }
        return false;
    }
    uint64_t end = cpu::read_tsc() + us * mhz;
    uint32_t n = 0;
    while (cpu::read_tsc() < end) {
        if (stop && (++n & 255) == 0 && stop()) return true;
        cpu::pause();
    }
    return false;
}

bool note(uint32_t from_hz, uint32_t to_hz, uint32_t ms, uint32_t vibrato, bool (*stop)()) {
    if (!g_enabled) return false;
    constexpr uint32_t kStepMs = 4;  // the pitch is updated this often (glides and vibrato)
    for (uint32_t t = 0; t < ms; t += kStepMs) {
        int64_t hz = int64_t(from_hz) + (int64_t(to_hz) - int64_t(from_hz)) * t / (ms ? ms : 1);
        if (vibrato) {  // ~6 Hz wobble, `vibrato` cents deep (1 cent ≈ 1/1731 of the pitch)
            int32_t wobble = triangle(t * 6);
            hz += hz * wobble * int64_t(vibrato) / (1000 * 1731);
        }
        tone(uint32_t(hz));
        if (wait_us(kStepMs * 1000, stop)) {
            off();
            return true;
        }
    }
    off();
    return false;
}

void beep(uint32_t hz, uint32_t ms) {
    if (!g_enabled) return;
    if (ms > 10000) ms = 10000;
    tone(hz);
    uint64_t flags = cpu::save_and_disable_interrupts();
    cpu::restore_interrupts(flags);
    if ((flags & (1u << 9)) && sched::running()) sched::sleep_ms(ms);
    else wait_us(uint64_t(ms) * 1000);
    off();
}

void play_stop_tune(bool (*stop)()) {
    if (!g_enabled) return;
    struct Note {
        uint16_t from, to, ms, vibrato, gap;
    };
    static const Note kTune[] = {
        {520, 860, 110, 0, 0},     // "mi-"
        {860, 430, 380, 25, 220},  // "-aaw" (the sad meow)
        {392, 384, 300, 0, 70},    // wah
        {370, 362, 300, 0, 70},    // wah
        {349, 340, 300, 0, 70},    // wah
        {330, 330, 650, 60, 0},    // waaaah…
        {330, 294, 350, 60, 0},    // …and down
    };
    for (const Note& n : kTune) {
        if (note(n.from, n.to, n.ms, n.vibrato, stop)) return;
        if (n.gap && wait_us(uint64_t(n.gap) * 1000, stop)) return;
    }
}

}  // namespace neko::speaker
