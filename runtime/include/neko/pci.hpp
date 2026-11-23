// PCI / PCIe devices (arch/x86_64/pci.cpp; empty in host mode).
//
// The configuration space is reached through ECAM (memory-mapped, from the ACPI MCFG table) or, when
// the firmware has no MCFG, through the legacy ports 0xCF8/0xCFC (first 256 bytes only). Buses are
// scanned once at boot, recursively through PCI-to-PCI bridges; drivers then look devices up by
// vendor/device id or by class and talk to them through the index of the table.
#pragma once

#include <neko/types.hpp>

namespace neko::pci {

enum BarFlags : uint8_t {
    kBarIo = 1,            // I/O ports instead of memory
    kBar64 = 2,            // 64-bit memory BAR (uses the next slot too)
    kBarPrefetchable = 4,
};

// Field names match sdk/Nekotia.Sdk/Hal/Pci.cs.
struct Device {
    uint16_t segment;
    uint8_t bus, device, function;
    uint8_t header_type;  // 0 device, 1 PCI-to-PCI bridge, 2 CardBus bridge
    uint16_t vendor_id, device_id;
    uint8_t class_code, subclass, prog_if, revision;
    uint16_t subsystem_vendor, subsystem_id;
    uint8_t irq_line, irq_pin;  // legacy INTx routing set by the firmware (pin 0 = none)
    uint64_t bar_address[6];    // physical address or I/O port; 0 = unused
    uint64_t bar_size[6];
    uint8_t bar_flags[6];
};

constexpr uint32_t kMaxDevices = 64;

// Configuration space access mechanisms, filled from the MCFG before init().
void add_ecam(uint64_t phys, uint16_t segment, uint8_t start_bus, uint8_t end_bus);
void init();  // scans every bus (needs the VMM for ECAM)

bool uses_ecam();
uint32_t count();
bool info(uint32_t index, Device& out);
const Device* get(uint32_t index);  // nullptr past the end

// Index of the next matching device at or after `start`, or -1. 0xFFFF / 0xFF match anything.
int32_t find(uint16_t vendor_id, uint16_t device_id, uint32_t start = 0);
int32_t find_class(uint8_t class_code, uint8_t subclass, uint32_t start = 0);

// Configuration registers of device `index` (offsets must be aligned to the access size).
uint32_t read32(uint32_t index, uint16_t offset);
uint16_t read16(uint32_t index, uint16_t offset);
uint8_t read8(uint32_t index, uint16_t offset);
void write32(uint32_t index, uint16_t offset, uint32_t value);
void write16(uint32_t index, uint16_t offset, uint16_t value);

// Command register: memory / I/O decoding and bus mastering (DMA).
void enable(uint32_t index, bool memory, bool io, bool bus_master);
// Offset of capability `id` in the capability list (0x05 MSI, 0x10 PCIe, 0x11 MSI-X, 0x09 vendor), 0 if absent.
uint8_t find_capability(uint32_t index, uint8_t id, uint8_t after = 0);
// Maps memory BAR `bar` uncached and returns its virtual address (nullptr for I/O or empty BARs).
void* map_bar(uint32_t index, uint32_t bar);

}  // namespace neko::pci
