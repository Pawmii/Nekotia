// USB (drivers/xhci.cpp): the xHCI host controller, devices on its root ports, and HID boot-protocol
// keyboards and mice. Key presses are turned into PS/2 scancodes (set 1) and handed to the same
// keyboard driver that serves the PS/2 port (kernel/Drivers/Keyboard.neko), so layouts, Ctrl+C and
// the rest work unchanged. Hubs are not supported yet: devices must sit on root ports.
#pragma once

#include <neko/types.hpp>

namespace neko::usb {

// Field names match sdk/Nekotia.Sdk/Hal/Usb.cs.
struct Info {
    uint8_t slot;
    uint8_t port;
    uint8_t speed;        // 1 full, 2 low, 3 high, 4 super (xHCI port speed ids)
    uint8_t kind;         // 0 other, 1 keyboard, 2 mouse
    uint16_t vendor_id, product_id;
    uint8_t class_code, subclass, protocol;  // of the device, or of the HID interface
    uint8_t product[40];  // from the string descriptor, if any
    uint64_t reports;     // HID reports received
};

uint32_t count();
bool info(uint32_t index, Info& out);

// Where keyboard scancodes go (the C# keyboard driver). Called from the controller's interrupt handler.
using ScancodeSink = void (*)(uint8_t scancode);
void set_keyboard_sink(ScancodeSink sink);

// Accumulated mouse state from every USB mouse.
struct Mouse {
    int64_t x, y;      // sum of the movements since boot
    int64_t wheel;
    uint8_t buttons;   // bit 0 left, 1 right, 2 middle
    uint64_t reports;
};
Mouse mouse();

}  // namespace neko::usb
