// Formatting on top of the platform's serial::write_char, plus C string helpers.
#include <neko/runtime.hpp>

namespace neko::serial {

void write(const char* s) {
    if (!s) s = "(null)";
    while (*s) write_char(static_cast<uint8_t>(*s++));
}

void write(const uint8_t* s) { write(reinterpret_cast<const char*>(s)); }

void write_hex(uint64_t value) {
    static constexpr char kDigits[] = "0123456789abcdef";
    write("0x");
    bool started = false;
    for (int shift = 60; shift >= 0; shift -= 4) {
        unsigned digit = (value >> shift) & 0xF;
        if (digit || started || shift == 0) {
            write_char(static_cast<uint8_t>(kDigits[digit]));
            started = true;
        }
    }
}

void write_udec(uint64_t value) {
    char buf[21];
    int i = 0;
    do {
        buf[i++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value);
    while (i) write_char(static_cast<uint8_t>(buf[--i]));
}

void write_dec(int64_t value) {
    if (value < 0) {
        write_char('-');
        write_udec(static_cast<uint64_t>(0) - static_cast<uint64_t>(value));
    } else {
        write_udec(static_cast<uint64_t>(value));
    }
}

void write_size(uint64_t bytes) {
    static constexpr const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int unit = 0;
    uint64_t divisor = 1;
    while (bytes / divisor >= 1024 && unit < 4) {
        divisor *= 1024;
        ++unit;
    }
    // Round to tenths; print the decimal only when it carries information ("1.5 MiB", "16 MiB").
    uint64_t tenths = (bytes / divisor) * 10 + ((bytes % divisor) * 10 + divisor / 2) / divisor;
    write_udec(tenths / 10);
    if (unit > 0 && tenths % 10 != 0 && tenths < 1000) {
        write_char('.');
        write_udec(tenths % 10);
    }
    write_char(' ');
    write(kUnits[unit]);
}

}  // namespace neko::serial

namespace neko::str {

size_t length(const char* s) {
    size_t n = 0;
    while (s && s[n]) ++n;
    return n;
}

bool equals(const char* a, const char* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    while (*a && *a == *b) ++a, ++b;
    return *a == *b;
}

bool equals(const uint8_t* a, const char* b) { return equals(reinterpret_cast<const char*>(a), b); }

}  // namespace neko::str

namespace neko {

void panic(const char* message) {
    static bool in_panic = false;
    if (!in_panic) {
        in_panic = true;
        serial::write("\n*** NEKOTIA PANIC: ");
        serial::write(message);
        serial::write("\n");
    }
    platform::stop_after_panic();
}

}  // namespace neko
