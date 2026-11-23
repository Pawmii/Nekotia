// PCI enumeration: ECAM (ACPI MCFG) or the legacy 0xCF8/0xCFC ports, a recursive bus scan through
// bridges, BAR decoding with size probing, and configuration-space helpers for drivers.
#include <neko/pci.hpp>
#include <neko/runtime.hpp>

namespace neko::pci {
namespace {

struct Ecam {
    uint64_t phys;
    uint16_t segment;
    uint8_t start_bus, end_bus;
};

Ecam g_ecam[4];
uint32_t g_ecam_count = 0;
Device g_devices[kMaxDevices];
uint32_t g_count = 0;
bool g_scanned_buses[256];
sync::SpinLock g_legacy_lock;  // 0xCF8 address + 0xCFC data must not interleave

// Virtual address of the 4 KiB configuration space of (seg, bus, dev, fn), or nullptr without ECAM.
volatile uint8_t* ecam_address(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t fn) {
    for (uint32_t i = 0; i < g_ecam_count; ++i) {
        const Ecam& e = g_ecam[i];
        if (e.segment != seg || bus < e.start_bus || bus > e.end_bus) continue;
        uint64_t phys = e.phys + (uint64_t(bus - e.start_bus) << 20) + (uint64_t(dev) << 15) + (uint64_t(fn) << 12);
        return static_cast<volatile uint8_t*>(vmm::map_mmio(phys, 4096));
    }
    return nullptr;
}

uint32_t raw_read32(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off) {
    if (volatile uint8_t* base = ecam_address(seg, bus, dev, fn))
        return *reinterpret_cast<volatile uint32_t*>(base + off);
    if (seg != 0 || off >= 256) return 0xFFFFFFFF;
    sync::ScopedLock guard(g_legacy_lock);
    cpu::outl(0xCF8, 0x80000000u | (uint32_t(bus) << 16) | (uint32_t(dev) << 11) | (uint32_t(fn) << 8) | (off & 0xFC));
    return cpu::inl(0xCFC);
}

void raw_write32(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off, uint32_t value) {
    if (volatile uint8_t* base = ecam_address(seg, bus, dev, fn)) {
        *reinterpret_cast<volatile uint32_t*>(base + off) = value;
        return;
    }
    if (seg != 0 || off >= 256) return;
    sync::ScopedLock guard(g_legacy_lock);
    cpu::outl(0xCF8, 0x80000000u | (uint32_t(bus) << 16) | (uint32_t(dev) << 11) | (uint32_t(fn) << 8) | (off & 0xFC));
    cpu::outl(0xCFC, value);
}

void probe_bars(Device& d) {
    uint32_t bars = d.header_type == 0 ? 6 : d.header_type == 1 ? 2 : 0;
    // Decoding off while the BARs hold all-ones, so the device does not answer random addresses.
    uint32_t command = raw_read32(d.segment, d.bus, d.device, d.function, 0x04);
    raw_write32(d.segment, d.bus, d.device, d.function, 0x04, command & ~0x3u);
    for (uint32_t i = 0; i < bars; ++i) {
        uint16_t off = uint16_t(0x10 + i * 4);
        uint32_t original = raw_read32(d.segment, d.bus, d.device, d.function, off);
        raw_write32(d.segment, d.bus, d.device, d.function, off, 0xFFFFFFFF);
        uint32_t mask = raw_read32(d.segment, d.bus, d.device, d.function, off);
        raw_write32(d.segment, d.bus, d.device, d.function, off, original);
        if (mask == 0 || mask == 0xFFFFFFFF) continue;  // not implemented

        if (original & 1) {  // I/O
            d.bar_flags[i] = kBarIo;
            d.bar_address[i] = original & ~0x3u;
            d.bar_size[i] = uint16_t(~(mask & ~0x3u) + 1);
            continue;
        }
        uint64_t address = original & ~0xFu;
        uint64_t size_mask = 0xFFFFFFFF00000000ull | (mask & ~0xFu);
        uint8_t flags = (original & 0x8) ? kBarPrefetchable : 0;
        if (((original >> 1) & 3) == 2 && i + 1 < bars) {  // 64-bit: the next BAR holds the high half
            uint16_t hi = uint16_t(off + 4);
            uint32_t original_hi = raw_read32(d.segment, d.bus, d.device, d.function, hi);
            raw_write32(d.segment, d.bus, d.device, d.function, hi, 0xFFFFFFFF);
            uint32_t mask_hi = raw_read32(d.segment, d.bus, d.device, d.function, hi);
            raw_write32(d.segment, d.bus, d.device, d.function, hi, original_hi);
            address |= uint64_t(original_hi) << 32;
            size_mask = (uint64_t(mask_hi) << 32) | (mask & ~0xFu);
            flags |= kBar64;
        }
        d.bar_flags[i] = flags;
        d.bar_address[i] = address;
        d.bar_size[i] = ~size_mask + 1;
        if (flags & kBar64) ++i;
    }
    raw_write32(d.segment, d.bus, d.device, d.function, 0x04, command);
}

void scan_bus(uint16_t seg, uint8_t bus);

void scan_function(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t fn) {
    uint32_t id = raw_read32(seg, bus, dev, fn, 0x00);
    if ((id & 0xFFFF) == 0xFFFF) return;
    uint32_t class_reg = raw_read32(seg, bus, dev, fn, 0x08);
    uint32_t header = raw_read32(seg, bus, dev, fn, 0x0C);
    uint8_t type = uint8_t((header >> 16) & 0x7F);

    if (g_count < kMaxDevices) {
        Device& d = g_devices[g_count++];
        mem::set(&d, 0, sizeof(d));
        d.segment = seg;
        d.bus = bus;
        d.device = dev;
        d.function = fn;
        d.header_type = type;
        d.vendor_id = uint16_t(id);
        d.device_id = uint16_t(id >> 16);
        d.revision = uint8_t(class_reg);
        d.prog_if = uint8_t(class_reg >> 8);
        d.subclass = uint8_t(class_reg >> 16);
        d.class_code = uint8_t(class_reg >> 24);
        if (type == 0) {
            uint32_t sub = raw_read32(seg, bus, dev, fn, 0x2C);
            d.subsystem_vendor = uint16_t(sub);
            d.subsystem_id = uint16_t(sub >> 16);
        }
        uint32_t irq = raw_read32(seg, bus, dev, fn, 0x3C);
        d.irq_line = uint8_t(irq);
        d.irq_pin = uint8_t(irq >> 8);
        probe_bars(d);
    }

    if (type == 1) {  // PCI-to-PCI bridge: its secondary bus
        uint8_t secondary = uint8_t(raw_read32(seg, bus, dev, fn, 0x18) >> 8);
        if (secondary != 0 && secondary != bus) scan_bus(seg, secondary);
    }
}

void scan_bus(uint16_t seg, uint8_t bus) {
    if (g_scanned_buses[bus]) return;  // a misconfigured bridge must not loop forever
    g_scanned_buses[bus] = true;
    for (uint8_t dev = 0; dev < 32; ++dev) {
        uint32_t id = raw_read32(seg, bus, dev, 0, 0x00);
        if ((id & 0xFFFF) == 0xFFFF) continue;
        bool multi = (raw_read32(seg, bus, dev, 0, 0x0C) >> 16) & 0x80;
        for (uint8_t fn = 0; fn < (multi ? 8 : 1); ++fn) scan_function(seg, bus, dev, fn);
    }
}

const Device* dev_at(uint32_t index) { return index < g_count ? &g_devices[index] : nullptr; }

}  // namespace

void add_ecam(uint64_t phys, uint16_t segment, uint8_t start_bus, uint8_t end_bus) {
    if (g_ecam_count < 4 && end_bus >= start_bus) g_ecam[g_ecam_count++] = {phys, segment, start_bus, end_bus};
}

void init() {
    g_count = 0;
    mem::set(g_scanned_buses, 0, sizeof(g_scanned_buses));
    // Segment 0 only: QEMU and ordinary PCs have one; host bridges with several functions are
    // separate root buses (bus number = function).
    uint32_t root_header = raw_read32(0, 0, 0, 0, 0x0C);
    if ((raw_read32(0, 0, 0, 0, 0) & 0xFFFF) == 0xFFFF) return;
    if ((root_header >> 16) & 0x80) {
        for (uint8_t fn = 0; fn < 8; ++fn)
            if ((raw_read32(0, 0, 0, fn, 0) & 0xFFFF) != 0xFFFF) scan_bus(0, fn);
    } else {
        scan_bus(0, 0);
    }
}

bool uses_ecam() { return g_ecam_count > 0; }
uint32_t count() { return g_count; }

bool info(uint32_t index, Device& out) {
    const Device* d = dev_at(index);
    if (!d) return false;
    out = *d;
    return true;
}

const Device* get(uint32_t index) { return dev_at(index); }

int32_t find(uint16_t vendor_id, uint16_t device_id, uint32_t start) {
    for (uint32_t i = start; i < g_count; ++i) {
        const Device& d = g_devices[i];
        if ((vendor_id == 0xFFFF || d.vendor_id == vendor_id) && (device_id == 0xFFFF || d.device_id == device_id))
            return int32_t(i);
    }
    return -1;
}

int32_t find_class(uint8_t class_code, uint8_t subclass, uint32_t start) {
    for (uint32_t i = start; i < g_count; ++i) {
        const Device& d = g_devices[i];
        if ((class_code == 0xFF || d.class_code == class_code) && (subclass == 0xFF || d.subclass == subclass))
            return int32_t(i);
    }
    return -1;
}

uint32_t read32(uint32_t index, uint16_t offset) {
    const Device* d = dev_at(index);
    return d ? raw_read32(d->segment, d->bus, d->device, d->function, offset & 0xFFC) : 0xFFFFFFFF;
}

uint16_t read16(uint32_t index, uint16_t offset) {
    return uint16_t(read32(index, offset & ~3) >> ((offset & 2) * 8));
}

uint8_t read8(uint32_t index, uint16_t offset) {
    return uint8_t(read32(index, offset & ~3) >> ((offset & 3) * 8));
}

void write32(uint32_t index, uint16_t offset, uint32_t value) {
    const Device* d = dev_at(index);
    if (d) raw_write32(d->segment, d->bus, d->device, d->function, offset & 0xFFC, value);
}

void write16(uint32_t index, uint16_t offset, uint16_t value) {
    uint32_t shift = (offset & 2) * 8;
    uint32_t v = read32(index, offset & ~3);
    v = (v & ~(0xFFFFu << shift)) | (uint32_t(value) << shift);
    write32(index, offset & ~3, v);
}

void enable(uint32_t index, bool memory, bool io, bool bus_master) {
    uint16_t command = read16(index, 0x04);
    if (io) command |= 1;
    if (memory) command |= 2;
    if (bus_master) command |= 4;
    write16(index, 0x04, command);
}

uint8_t find_capability(uint32_t index, uint8_t id, uint8_t after) {
    if (!(read16(index, 0x06) & 0x10)) return 0;  // status: no capability list
    uint8_t at = after ? read8(index, uint16_t(after + 1)) : read8(index, 0x34);
    for (int guard = 0; at >= 0x40 && guard < 48; ++guard) {
        at &= 0xFC;
        if (read8(index, at) == id) return at;
        at = read8(index, uint16_t(at + 1));
    }
    return 0;
}

namespace {
volatile uint32_t* msix_table(uint32_t index, uint8_t cap) {
    uint32_t table = read32(index, uint16_t(cap + 4));
    uint8_t* bar = static_cast<uint8_t*>(map_bar(index, table & 7));
    return bar ? reinterpret_cast<volatile uint32_t*>(bar + (table & ~7u)) : nullptr;
}
}  // namespace

uint32_t msix_enable(uint32_t index) {
    uint8_t cap = find_capability(index, 0x11);
    if (!cap) return 0;
    uint16_t control = read16(index, uint16_t(cap + 2));
    uint32_t size = (control & 0x7FF) + 1;
    volatile uint32_t* table = msix_table(index, cap);
    if (!table) return 0;
    enable(index, true, false, false);
    for (uint32_t i = 0; i < size; ++i) table[i * 4 + 3] = 1;  // masked until routed
    write16(index, uint16_t(cap + 2), uint16_t((control | 0x8000) & ~0x4000));  // enable, unmask the function
    write16(index, 0x04, uint16_t(read16(index, 0x04) | 0x400));               // INTx off
    return size;
}

bool msix_route(uint32_t index, uint32_t entry, uint8_t vector) {
    uint8_t cap = find_capability(index, 0x11);
    if (!cap || entry > (read16(index, uint16_t(cap + 2)) & 0x7FFu)) return false;
    volatile uint32_t* e = msix_table(index, cap);
    if (!e) return false;
    e += entry * 4;
    uint64_t address = irq::msi_address();
    e[0] = uint32_t(address);
    e[1] = uint32_t(address >> 32);
    e[2] = irq::msi_data(vector);
    e[3] = 0;  // unmask
    return true;
}

bool msi_enable(uint32_t index, uint8_t vector) {
    uint8_t cap = find_capability(index, 0x05);
    if (!cap) return false;
    uint16_t control = read16(index, uint16_t(cap + 2));
    uint64_t address = irq::msi_address();
    write32(index, uint16_t(cap + 4), uint32_t(address));
    uint16_t data_at = uint16_t(cap + 8);
    if (control & 0x80) {  // 64-bit address
        write32(index, uint16_t(cap + 8), uint32_t(address >> 32));
        data_at = uint16_t(cap + 12);
    }
    write16(index, data_at, uint16_t(irq::msi_data(vector)));
    write16(index, uint16_t(cap + 2), uint16_t((control & ~0x70u) | 1));  // one vector, enabled
    write16(index, 0x04, uint16_t(read16(index, 0x04) | 0x400));         // INTx off
    return true;
}

void* map_bar(uint32_t index, uint32_t bar) {
    const Device* d = dev_at(index);
    if (!d || bar >= 6 || (d->bar_flags[bar] & kBarIo) || d->bar_address[bar] == 0) return nullptr;
    return vmm::map_mmio(d->bar_address[bar], d->bar_size[bar]);
}

}  // namespace neko::pci
