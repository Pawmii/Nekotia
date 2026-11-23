// The handful of libc symbols clang may emit calls to in freestanding code.
// Implemented with `rep movsb/stosb` so the optimizer cannot turn them back into calls to themselves.
#include <neko/types.hpp>

extern "C" {

void* memcpy(void* dst, const void* src, size_t n) {
    void* ret = dst;
    __asm__ volatile("rep movsb" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
    return ret;
}

void* memset(void* dst, int value, size_t n) {
    void* ret = dst;
    __asm__ volatile("rep stosb" : "+D"(dst), "+c"(n) : "a"(value) : "memory");
    return ret;
}

void* memmove(void* dst, const void* src, size_t n) {
    auto* d = static_cast<uint8_t*>(dst);
    auto* s = static_cast<const uint8_t*>(src);
    if (d <= s || d >= s + n) return memcpy(dst, src, n);
    // Overlap with dst after src: copy backwards.
    d += n - 1;
    s += n - 1;
    __asm__ volatile("std; rep movsb; cld" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
    auto* x = static_cast<const uint8_t*>(a);
    auto* y = static_cast<const uint8_t*>(b);
    for (size_t i = 0; i < n; ++i)
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

}  // extern "C"
