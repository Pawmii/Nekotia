// ACPI table discovery: RSDP → XSDT/RSDT → MADT ("APIC") for the interrupt controllers.
#include "arch.hpp"

namespace neko::arch {
namespace {

struct __attribute__((packed)) Rsdp {
    char signature[8];
    uint8_t checksum;
    char oem[6];
    uint8_t revision;
    uint32_t rsdt;
    uint32_t length;
    uint64_t xsdt;
    uint8_t extended_checksum;
    uint8_t reserved[3];
};

struct __attribute__((packed)) SdtHeader {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem[6];
    char oem_table[8];
    uint32_t oem_revision;
    uint32_t creator;
    uint32_t creator_revision;
};

struct __attribute__((packed)) Madt {
    SdtHeader header;
    uint32_t lapic_address;
    uint32_t flags;
};

const SdtHeader* map_table(uint64_t phys) {
    auto* h = static_cast<const SdtHeader*>(vmm::map_mmio(phys, sizeof(SdtHeader)));
    vmm::map_mmio(phys, h->length);
    return h;
}

bool checksum_ok(const void* p, size_t n) {
    uint8_t sum = 0;
    for (size_t i = 0; i < n; ++i) sum = uint8_t(sum + static_cast<const uint8_t*>(p)[i]);
    return sum == 0;
}

bool sig(const char* a, const char* b) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3]; }

void parse_madt(const Madt* madt, MadtInfo& out) {
    out.lapic_phys = madt->lapic_address;
    auto* p = reinterpret_cast<const uint8_t*>(madt) + sizeof(Madt);
    auto* end = reinterpret_cast<const uint8_t*>(madt) + madt->header.length;
    while (p + 2 <= end && p[1] >= 2) {
        switch (p[0]) {
            case 0: {  // processor local APIC
                uint32_t flags = *reinterpret_cast<const uint32_t*>(p + 4);
                if (flags & 0x3) ++out.cpu_count;  // enabled or online-capable
                break;
            }
            case 1:  // IOAPIC
                if (out.ioapic_count < 4) {
                    auto& io = out.ioapics[out.ioapic_count++];
                    io.id = p[2];
                    io.phys = *reinterpret_cast<const uint32_t*>(p + 4);
                    io.gsi_base = *reinterpret_cast<const uint32_t*>(p + 8);
                }
                break;
            case 2:  // interrupt source override (ISA IRQ → GSI)
                if (out.override_count < 16) {
                    auto& o = out.overrides[out.override_count++];
                    o.source = p[3];
                    o.gsi = *reinterpret_cast<const uint32_t*>(p + 4);
                    o.flags = *reinterpret_cast<const uint16_t*>(p + 8);
                }
                break;
            case 5:  // 64-bit LAPIC address override
                out.lapic_phys = *reinterpret_cast<const uint64_t*>(p + 4);
                break;
        }
        p += p[1];
    }
}

}  // namespace

bool acpi_init(void* rsdp_ptr, uint64_t hhdm, MadtInfo& out) {
    if (!rsdp_ptr) return false;
    // Limine base revision 3 hands over a physical address; older ones a virtual one.
    uint64_t addr = reinterpret_cast<uint64_t>(rsdp_ptr);
    uint64_t phys = addr >= hhdm ? addr - hhdm : addr;
    auto* rsdp = static_cast<const Rsdp*>(vmm::map_mmio(phys, sizeof(Rsdp)));
    if (!checksum_ok(rsdp, 20)) return false;

    bool xsdt = rsdp->revision >= 2 && rsdp->xsdt;
    const SdtHeader* root = map_table(xsdt ? rsdp->xsdt : rsdp->rsdt);
    size_t entries = (root->length - sizeof(SdtHeader)) / (xsdt ? 8 : 4);
    auto* list = reinterpret_cast<const uint8_t*>(root) + sizeof(SdtHeader);

    for (size_t i = 0; i < entries; ++i) {
        uint64_t table_phys = xsdt ? reinterpret_cast<const uint64_t*>(list)[i] : reinterpret_cast<const uint32_t*>(list)[i];
        const SdtHeader* h = map_table(table_phys);
        if (sig(h->signature, "APIC")) {
            parse_madt(reinterpret_cast<const Madt*>(h), out);
            return out.ioapic_count > 0;
        }
    }
    return false;
}

}  // namespace neko::arch
