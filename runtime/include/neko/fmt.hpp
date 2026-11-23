// Numbers → text in a caller's buffer, header-only: shared by the kernel, user programs and the code
// nekoc generates for interpolation alignment (`{value,8}`). Every function NUL-terminates and returns
// the length. Buffers: 24 bytes for integers and sizes, 40 for fixed-point floats.
#pragma once

#include <neko/types.hpp>

namespace neko::fmt {

inline int udec(char* out, uint64_t v) {
    char tmp[24];
    int n = 0;
    do {
        tmp[n++] = char('0' + v % 10);
        v /= 10;
    } while (v);
    for (int i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
    out[n] = 0;
    return n;
}

inline int dec(char* out, int64_t v) {
    if (v >= 0) return udec(out, uint64_t(v));
    out[0] = '-';
    return 1 + udec(out + 1, uint64_t(0) - uint64_t(v));
}

// At least `digits` digits, zero-padded (a minus sign comes first).
inline int dec_digits(char* out, int64_t v, int digits) {
    int n = 0;
    uint64_t u = uint64_t(v);
    if (v < 0) {
        out[n++] = '-';
        u = uint64_t(0) - uint64_t(v);
    }
    char tmp[24];
    int len = udec(tmp, u);
    for (int i = len; i < digits && n < 20; ++i) out[n++] = '0';
    for (int i = 0; i <= len; ++i) out[n + i] = tmp[i];
    return n + len;
}

inline int hex_digits(char* out, uint64_t v, int digits) {
    if (digits < 1) digits = 1;
    if (digits > 16) digits = 16;
    for (int i = 0; i < digits; ++i) out[i] = "0123456789abcdef"[(v >> ((digits - 1 - i) * 4)) & 15];
    out[digits] = 0;
    return digits;
}

inline int hex(char* out, uint64_t v) {
    int digits = 1;
    while (digits < 16 && (v >> (digits * 4))) ++digits;
    out[0] = '0';
    out[1] = 'x';
    return 2 + hex_digits(out + 2, v, digits);
}

inline int size(char* out, uint64_t bytes) {
    static constexpr const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int unit = 0;
    uint64_t divisor = 1;
    while (bytes / divisor >= 1024 && unit < 4) {
        divisor *= 1024;
        ++unit;
    }
    uint64_t tenths = (bytes / divisor) * 10 + ((bytes % divisor) * 10 + divisor / 2) / divisor;
    int n = udec(out, tenths / 10);
    if (unit > 0 && tenths % 10 != 0 && tenths < 1000) {
        out[n++] = '.';
        out[n++] = char('0' + tenths % 10);
    }
    out[n++] = ' ';
    for (const char* u = kUnits[unit]; *u;) out[n++] = *u++;
    out[n] = 0;
    return n;
}

inline int fixed(char* out, double v, int decimals) {
    int n = 0;
    if (v != v) {
        out[0] = 'N', out[1] = 'a', out[2] = 'N', out[3] = 0;
        return 3;
    }
    if (v < 0) {
        out[n++] = '-';
        v = -v;
    }
    if (decimals < 0) decimals = 0;
    if (decimals > 9) decimals = 9;
    if (v > 1.8e19) {
        out[n++] = 'i', out[n++] = 'n', out[n++] = 'f', out[n] = 0;
        return n;
    }
    uint64_t scale = 1;
    for (int i = 0; i < decimals; ++i) scale *= 10;
    uint64_t whole = uint64_t(v);
    uint64_t frac = uint64_t((v - double(whole)) * double(scale) + 0.5);
    if (frac >= scale) ++whole, frac -= scale;
    n += udec(out + n, whole);
    if (decimals) {
        out[n++] = '.';
        for (uint64_t d = scale / 10; d > 1 && frac < d; d /= 10) out[n++] = '0';
        n += udec(out + n, frac);
    }
    out[n] = 0;
    return n;
}

// Characters of a UTF-8 string (what alignment counts).
inline int width(const char* s) {
    int n = 0;
    for (; s && *s; ++s)
        if ((uint8_t(*s) & 0xC0) != 0x80) ++n;
    return n;
}

// byte* overloads for C#.
inline int udec(uint8_t* o, uint64_t v) { return udec(reinterpret_cast<char*>(o), v); }
inline int dec(uint8_t* o, int64_t v) { return dec(reinterpret_cast<char*>(o), v); }
inline int hex(uint8_t* o, uint64_t v) { return hex(reinterpret_cast<char*>(o), v); }
inline int hex_digits(uint8_t* o, uint64_t v, int d) { return hex_digits(reinterpret_cast<char*>(o), v, d); }
inline int size(uint8_t* o, uint64_t b) { return size(reinterpret_cast<char*>(o), b); }
inline int fixed(uint8_t* o, double v, int d) { return fixed(reinterpret_cast<char*>(o), v, d); }
inline int width(const uint8_t* s) { return width(reinterpret_cast<const char*>(s)); }

}  // namespace neko::fmt
