// USB (drivers/xhci.cpp): the xHCI host controller, hubs (USB 2 and 3, any depth), hot plug, HID
// boot-protocol keyboards and mice, and mass storage (USB sticks: Bulk-Only Transport + SCSI, as block
// devices usb0, usb1 … that the kernel mounts under /media). Key presses are turned into PS/2 scancodes
// (set 1) and handed to the same keyboard driver that serves the PS/2 port (kernel/Drivers/Keyboard.neko),
// so layouts, Ctrl+C and the rest work unchanged.
#pragma once

#include <neko/types.hpp>

namespace neko::usb {

// Field names match sdk/Nekotia.Sdk/Hal/Usb.cs.
struct Info {
    uint8_t slot;
    uint8_t port;
    uint8_t speed;        // 1 full, 2 low, 3 high, 4 super (xHCI port speed ids)
    uint8_t kind;         // 0 other, 1 keyboard, 2 mouse, 3 hub, 4 storage
    uint16_t vendor_id, product_id;
    uint8_t class_code, subclass, protocol;  // of the device, or of the HID interface
    uint8_t product[40];  // from the string descriptor, if any
    uint64_t reports;     // HID reports received
    uint8_t parent;       // slot of the hub it is plugged into, 0 = a root port
    uint8_t depth;        // hubs between it and the controller
    uint8_t hub_ports;    // hubs: downstream ports
    uint8_t reserved;
    uint8_t disk[8];      // storage: its block device ("usb0"), else empty
};

constexpr uint8_t kKindOther = 0, kKindKeyboard = 1, kKindMouse = 2, kKindHub = 3, kKindStorage = 4;

uint32_t count();
bool info(uint32_t index, Info& out);

// Where keyboard scancodes go (the C# keyboard driver). Called from the controller's interrupt handler.
using ScancodeSink = void (*)(uint8_t scancode);
void set_keyboard_sink(ScancodeSink sink);
// The stop screen (interrupts off, other CPUs halted): keyboard reports go to `sink` as set-1 scancodes; call
// it in a loop. Takes no locks and wakes nobody.
void emergency_poll(ScancodeSink sink);

// Accumulated mouse state from every USB mouse.
struct Mouse {
    int64_t x, y;      // sum of the movements since boot
    int64_t wheel;
    uint8_t buttons;   // bit 0 left, 1 right, 2 middle
    uint64_t reports;
};
Mouse mouse();

struct Stats {
    uint64_t attached, detached;  // devices since boot (hot plug)
};
Stats stats();

}  // namespace neko::usb
