// The PC speaker: PIT channel 2 drives a square wave on the speaker (port 0x61 gates it). One voice, no
// volume, but it needs no driver, works with interrupts off and is there on every PC (QEMU: `make run` passes
// -machine pcspk-audiodev=…, so it plays through the host's sound).
#pragma once

#include <neko/types.hpp>

namespace neko::speaker {

void configure(const char* cmdline);  // `nosound` on the kernel command line keeps it silent
bool enabled();

void tone(uint32_t hz);  // starts a square wave (0 = silence) and returns
void off();

// Busy-waits `us` microseconds (TSC; works with interrupts off). Returns early, true, when `stop` says so.
bool wait_us(uint64_t us, bool (*stop)() = nullptr);

// One note: `ms` long, sliding from `from_hz` to `to_hz` (equal for a plain note), `vibrato` in cents.
bool note(uint32_t from_hz, uint32_t to_hz, uint32_t ms, uint32_t vibrato = 0, bool (*stop)() = nullptr);

// A short tone that sleeps instead of spinning when it can (the shell's `beep`).
void beep(uint32_t hz, uint32_t ms);

// The stop screen's tune: a sad meow, then a "wah-wah-wah-waaah". `stop` is polled (the R key reboots).
void play_stop_tune(bool (*stop)() = nullptr);

}  // namespace neko::speaker
