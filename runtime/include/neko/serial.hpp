// Serial console (COM1). Formatting helpers live in runtime/src/core/print.cpp.
#pragma once

#include <neko/types.hpp>

namespace neko::serial {

void init();                       // platform specific
void write_char(uint8_t c);        // platform specific, translates '\n' → "\r\n" on hardware

void write(const char* s);
void write(const uint8_t* s);
void write_hex(uint64_t value);
void write_dec(int64_t value);
void write_udec(uint64_t value);
void write_size(uint64_t bytes);   // "12 KiB", "255 MiB"

}  // namespace neko::serial

namespace neko::str {

size_t length(const char* s);
size_t length(const uint8_t* s);
bool equals(const char* a, const char* b);
bool equals(const uint8_t* a, const char* b);
bool starts_with(const uint8_t* s, const char* prefix);

}  // namespace neko::str
