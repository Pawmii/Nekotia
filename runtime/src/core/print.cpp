// Number/string formatting shared by every output (serial only, or console + serial),
// plus C string helpers and panic.
#include <neko/console.hpp>
#include <neko/runtime.hpp>

namespace neko {
namespace {

using Sink = void (*)(uint8_t);

void put_str(Sink put, const char* s) {
    if (!s) s = "(null)";
    while (*s) put(static_cast<uint8_t>(*s++));
}

void put_hex(Sink put, uint64_t value) {
    static constexpr char kDigits[] = "0123456789abcdef";
    put_str(put, "0x");
    bool started = false;
    for (int shift = 60; shift >= 0; shift -= 4) {
        unsigned digit = (value >> shift) & 0xF;
        if (digit || started || shift == 0) {
            put(static_cast<uint8_t>(kDigits[digit]));
            started = true;
        }
    }
}

void put_udec(Sink put, uint64_t value) {
    char buf[21];
    int i = 0;
    do {
        buf[i++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value);
    while (i) put(static_cast<uint8_t>(buf[--i]));
}

void put_dec(Sink put, int64_t value) {
    if (value < 0) {
        put('-');
        put_udec(put, static_cast<uint64_t>(0) - static_cast<uint64_t>(value));
    } else {
        put_udec(put, static_cast<uint64_t>(value));
    }
}

void put_size(Sink put, uint64_t bytes) {
    static constexpr const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int unit = 0;
    uint64_t divisor = 1;
    while (bytes / divisor >= 1024 && unit < 4) {
        divisor *= 1024;
        ++unit;
    }
    // Round to tenths; print the decimal only when it carries information ("1.5 MiB", "16 MiB").
    uint64_t tenths = (bytes / divisor) * 10 + ((bytes % divisor) * 10 + divisor / 2) / divisor;
    put_udec(put, tenths / 10);
    if (unit > 0 && tenths % 10 != 0 && tenths < 1000) {
        put('.');
        put_udec(put, tenths % 10);
    }
    put(' ');
    put_str(put, kUnits[unit]);
}

void put_codepoint(Sink put, uint32_t cp) {
    if (cp < 0x80) {
        put(uint8_t(cp));
    } else if (cp < 0x800) {
        put(uint8_t(0xC0 | (cp >> 6)));
        put(uint8_t(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        put(uint8_t(0xE0 | (cp >> 12)));
        put(uint8_t(0x80 | ((cp >> 6) & 0x3F)));
        put(uint8_t(0x80 | (cp & 0x3F)));
    } else {
        put(uint8_t(0xF0 | (cp >> 18)));
        put(uint8_t(0x80 | ((cp >> 12) & 0x3F)));
        put(uint8_t(0x80 | ((cp >> 6) & 0x3F)));
        put(uint8_t(0x80 | (cp & 0x3F)));
    }
}

void put_float(Sink put, double v, int32_t decimals) {
    if (v != v) return put_str(put, "NaN");
    if (v < 0 || (v == 0 && __builtin_signbit(v))) {
        put('-');
        v = -v;
    }
    if (v > 1.8e19) return put_str(put, "∞");  // beyond uint64: good enough for kernel output
    if (decimals < 0) decimals = 0;
    if (decimals > 9) decimals = 9;
    uint64_t scale = 1;
    for (int i = 0; i < decimals; ++i) scale *= 10;
    uint64_t whole = uint64_t(v);
    uint64_t frac = uint64_t((v - double(whole)) * double(scale) + 0.5);
    if (frac >= scale) ++whole, frac -= scale;  // rounding carried into the integer part
    put_udec(put, whole);
    if (decimals == 0) return;
    put('.');
    for (uint64_t d = scale / 10; d > 1 && frac < d; d /= 10) put('0');
    put_udec(put, frac);
}

void term_put(uint8_t c) {
    serial::write_char(c);
    console::put_byte(c);
}

}  // namespace

namespace serial {
void write(const char* s) { put_str(write_char, s); }
void write(const uint8_t* s) { put_str(write_char, reinterpret_cast<const char*>(s)); }
void write_hex(uint64_t value) { put_hex(write_char, value); }
void write_dec(int64_t value) { put_dec(write_char, value); }
void write_udec(uint64_t value) { put_udec(write_char, value); }
void write_size(uint64_t bytes) { put_size(write_char, bytes); }
}  // namespace serial

namespace term {
void write_char(uint8_t c) { term_put(c); }
void write(const char* s) { put_str(term_put, s); }
void write(const uint8_t* s) { put_str(term_put, reinterpret_cast<const char*>(s)); }
void write_codepoint(uint32_t cp) { put_codepoint(term_put, cp); }
void write_hex(uint64_t value) { put_hex(term_put, value); }
void write_dec(int64_t value) { put_dec(term_put, value); }
void write_udec(uint64_t value) { put_udec(term_put, value); }
void write_size(uint64_t bytes) { put_size(term_put, bytes); }
void write_float(double value, int32_t decimals) { put_float(term_put, value, decimals); }
}  // namespace term

namespace str {

size_t length(const char* s) {
    size_t n = 0;
    while (s && s[n]) ++n;
    return n;
}

size_t length(const uint8_t* s) { return length(reinterpret_cast<const char*>(s)); }

bool equals(const char* a, const char* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    while (*a && *a == *b) ++a, ++b;
    return *a == *b;
}

bool equals(const uint8_t* a, const char* b) { return equals(reinterpret_cast<const char*>(a), b); }

bool starts_with(const uint8_t* s, const char* prefix) {
    if (!s || !prefix) return false;
    while (*prefix)
        if (*s++ != static_cast<uint8_t>(*prefix++)) return false;
    return true;
}

}  // namespace str

void panic(const char* message) { crash::panic_at(message, nullptr, 0, nullptr); }

}  // namespace neko
