// Input events (core/input.cpp): one stream of keyboard and pointer events for a display server.
//
//   PS/2 keyboard, USB keyboards ─► C# keyboard driver (kernel/Drivers/Keyboard.neko) ─┐
//   PS/2 mouse (kernel/Drivers/Mouse.neko), USB mice (xhci.cpp), virtio tablets ───────┴─► input::push ─► /dev/input
//
// Nobody reads /dev/input: keys go to the console as before, pointer events are dropped. Once a program
// opens /dev/input (exclusively — one reader at a time) it *grabs* the input: every key press and release
// goes to it as an Event (with the character the keyboard layout gives, so EN/RU switching keeps working),
// and the console gets nothing. Closing the device (or exiting) gives the keyboard back to the console.
//
// read() returns whole events (at most length / 32 of them), blocking until at least one is queued;
// poll() says when one is. Ctrl+Alt+Backspace ends the program that grabbed the input and the one that
// owns the screen — the way back to the console when a display server hangs.
#pragma once

#include <neko/types.hpp>

namespace neko::input {

// Event::type
constexpr uint16_t kKey = 1;       // code = key code, pressed, codepoint (0 for keys without a character)
constexpr uint16_t kMotion = 2;    // x, y = relative movement (y grows downwards)
constexpr uint16_t kButton = 3;    // code = button (0 left, 1 right, 2 middle), pressed
constexpr uint16_t kWheel = 4;     // y = notches (positive = towards the user / scroll down)
constexpr uint16_t kAbsolute = 5;  // x, y = position scaled to 0…32767 (tablets, touch screens)

// Event::mods (state at the time of the event)
constexpr uint8_t kModShift = 1, kModCtrl = 2, kModAlt = 4, kModCaps = 8, kModSuper = 16;

// Key codes are PS/2 scancode set 1 make codes; keys behind the 0xE0 prefix get 0x100 added
// (Up = 0x148, Right Ctrl = 0x11D…). Codepoints above 0x10FFFF are the console's special keys
// (Up = 0x110000…, see Keyboard.neko), control keys are their ASCII codes ('\n', '\b', '\t', 27).
struct Event {
    uint64_t time_ms;
    uint16_t type;
    uint16_t code;
    uint8_t pressed;
    uint8_t mods;
    uint16_t reserved;
    uint32_t codepoint;
    int32_t x, y;
    uint32_t reserved2;
};
static_assert(sizeof(Event) == 32);

void init();  // creates /dev/input (needs the VFS)

// Drivers (callable from interrupt handlers). Events are only queued while the input is grabbed.
bool grabbed();
void key(uint16_t code, bool pressed, uint8_t mods, uint32_t codepoint);
void motion(int32_t dx, int32_t dy);
// Button state of one device (bit 0 left, 1 right, 2 middle): events for the bits that changed since `last`,
// the device's own previous state (several pointing devices must not undo each other's presses).
void buttons(uint8_t& last, uint8_t mask);
void wheel(int32_t notches);
void absolute(int32_t x, int32_t y);  // 0…32767 each
// Ctrl+Alt+Backspace: kill the grabbing program and the owner of display 0.
void zap();

struct Stats {
    uint64_t events;   // queued since boot
    uint64_t dropped;  // the queue was full
    uint32_t grabbed_by;  // pid, 0 = nobody
};
Stats stats();

}  // namespace neko::input
