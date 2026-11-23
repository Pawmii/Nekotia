// The kernel log (core/log.cpp): a 64 KiB ring of timestamped, leveled messages — `dmesg`, /proc/log.
//
//   log::info("net: up")      → "[    1.234] info  net: up" in the ring; on screen (and serial) when its
//                               level is at least the threshold (loglevel=debug|info|warn|error on the kernel
//                               command line, default info); below it, serial only
//   boot capture              → until the shell starts, everything the terminal shows is kept too, so
//                               `dmesg` has the whole boot transcript
#pragma once

#include <neko/types.hpp>

namespace neko::log {

enum Level : int32_t { Debug = 0, Info = 1, Warn = 2, Error = 3 };

void write(Level level, const char* text);
inline void debug(const char* text) { write(Debug, text); }
inline void info(const char* text) { write(Info, text); }
inline void warn(const char* text) { write(Warn, text); }
inline void error(const char* text) { write(Error, text); }

void set_level(Level level);
Level level();
void configure(const char* cmdline);  // loglevel=…

// Terminal output is copied into the ring while capturing (on from boot until stop_capture()).
bool capturing();
void capture_byte(uint8_t c);
void stop_capture();

uint64_t size();  // bytes in the ring
// Copies `length` bytes starting `offset` bytes into the ring (oldest first); bytes copied.
int64_t read(uint64_t offset, uint8_t* buffer, uint64_t length);

}  // namespace neko::log
