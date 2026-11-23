// Matches PCI devices to drivers at boot (drivers::init, after interrupts and the VFS are up).
#include <neko/block.hpp>
#include <neko/runtime.hpp>

namespace neko::drivers {

void probe_virtio_blk(uint32_t pci);
void probe_virtio_net(uint32_t pci);
void probe_ahci(uint32_t pci);
void probe_nvme(uint32_t pci);
void probe_xhci(uint32_t pci);

void init() {
    for (uint32_t i = 0; i < pci::count(); ++i) {
        const pci::Device* d = pci::get(i);
        if (d->class_code == 0x01 && d->subclass == 0x06 && d->prog_if == 0x01) {  // SATA, AHCI 1.0
            probe_ahci(i);
            continue;
        }
        if (d->class_code == 0x01 && d->subclass == 0x08 && d->prog_if == 0x02) {  // NVM Express
            probe_nvme(i);
            continue;
        }
        if (d->class_code == 0x0C && d->subclass == 0x03 && d->prog_if == 0x30) {  // USB xHCI
            probe_xhci(i);
            continue;
        }
        if (d->vendor_id != 0x1AF4) continue;
        // Transitional ids 0x1000 (net) / 0x1001 (block); modern ones are 0x1040 + device type.
        if (d->device_id == 0x1001 || d->device_id == 0x1042) probe_virtio_blk(i);
        else if (d->device_id == 0x1000 || d->device_id == 0x1041) probe_virtio_net(i);
    }
}

}  // namespace neko::drivers
