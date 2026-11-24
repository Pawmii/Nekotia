// Sound: one output device (a driver attaches its DMA ring, drivers/hda.cpp), a software mixer, /dev/audio
// and a small synthesizer for tunes.
//
//   The format is fixed: 48 kHz, 16-bit signed little-endian, stereo (4 bytes a frame). The driver's ring
//   plays forever; the `audio` thread keeps it ~80 ms ahead of the play position with the mix of every
//   client (up to 4 programs that opened /dev/audio, a 64 KiB queue each) and the synthesizer, times the
//   master volume. With nothing to play it writes silence.
//
//   /dev/audio: write() queues PCM (blocks while the queue is full); close() lets what is queued finish.
//   ioctl: kAudioGetVolume → 0–100, kAudioSetVolume(arg), kAudioGetRate → 48000, kAudioDrain (waits until
//   this program's queue has played), kAudioGetQueued → bytes not yet mixed.
//
// Without a device (or in host mode) /dev/audio does not exist, and beep / the stop screen's tune fall back to
// the PC speaker (neko/speaker.hpp).
#pragma once

#include <neko/types.hpp>

namespace neko::audio {

constexpr uint32_t kRate = 48000;
constexpr uint32_t kFrame = 4;  // bytes: two 16-bit samples

constexpr uint64_t kAudioGetVolume = 0x4101, kAudioSetVolume = 0x4102, kAudioGetRate = 0x4103, kAudioDrain = 0x4104,
                   kAudioGetQueued = 0x4105;

struct Backend {
    const char* name;
    uint8_t* ring;                   // the DMA ring the device plays in a loop (kernel address)
    uint32_t bytes;                  // its size, a multiple of kFrame
    uint32_t (*position)(void* context);  // byte the device is reading now (works with interrupts off)
    void* context;
};

// A driver hands over its running output (once; later ones are ignored). Creates /dev/audio and the mixer thread.
bool attach(const Backend& backend);
bool available();
const char* device_name();

uint32_t volume();  // 0–100 (perceptual: the gain is (v/100)²)
void set_volume(uint32_t percent);

struct Stats {  // field names match SoundStats in sdk/Nekotia.Sdk/Hal/Splash.cs
    uint64_t Frames;     // mixed so far
    uint32_t Underruns;  // the device caught up with the mixer
    uint32_t Clients;
};
Stats stats();

// One note of a tune: sliding from `from_hz` to `to_hz` over `ms`, `vibrato` cents deep, then `gap_ms` of rest.
struct Note {
    uint16_t from_hz, to_hz, ms, vibrato, gap_ms;
};

// Plays a tune through the synthesizer (replacing one that plays). Returns at once.
bool play(const Note* notes, uint32_t count);
bool playing();

// A tone of `hz` for `ms`; sleeps meanwhile (PC speaker without a sound device).
void beep(uint32_t hz, uint32_t ms);

// The stop screen's tune: a sad meow and a wah-wah-wah-waaah. With interrupts off (the stop screen) it fills
// the device's ring itself while it waits; `stop` is polled (true ends it). PC speaker without a sound device.
void play_stop_tune(bool (*stop)() = nullptr);

}  // namespace neko::audio
