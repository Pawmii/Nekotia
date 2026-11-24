// bochs: QEMU's standard VGA (`-vga std`, PCI 1234:1111) and bochs-display, through the "DISPI" registers
// (I/O ports 0x1CE/0x1CF, or the MMIO block at 0x500 of BAR 2 where there are no legacy ports).
//
// The bootloader has already set the mode (the firmware's VBE / GOP drivers use DISPI too), and display 0
// shows it through bootfb. This driver takes display 0 over with the same memory and adds a second buffer:
// the virtual height becomes twice the screen and a flip is one register write (Y_OFFSET), so a display
// server draws into the hidden half and the screen never shows a half-drawn frame. QEMU has no vertical-blank
// interrupt for it, so frames are paced by the display layer's 60 Hz clock.
#include <neko/display.hpp>
#include <neko/runtime.hpp>

namespace neko::drivers {
namespace {

constexpr uint16_t kIndexPort = 0x01CE, kDataPort = 0x01CF;
constexpr uint16_t kRegId = 0, kRegXres = 1, kRegYres = 2, kRegBpp = 3, kRegEnable = 4, kRegVirtWidth = 6,
                   kRegVirtHeight = 7, kRegXOffset = 8, kRegYOffset = 9, kRegVideoMemory64K = 0xA;

struct Bochs {
    volatile uint16_t* mmio;  // DISPI registers in BAR 2 (index × 2), or nullptr: I/O ports
    uint32_t height;
};

Bochs g_bochs;

uint16_t read_reg(uint16_t index) {
    if (g_bochs.mmio) return g_bochs.mmio[index];
    cpu::outw(kIndexPort, index);
    return cpu::inw(kDataPort);
}

void write_reg(uint16_t index, uint16_t value) {
    if (g_bochs.mmio) {
        g_bochs.mmio[index] = value;
        return;
    }
    cpu::outw(kIndexPort, index);
    cpu::outw(kDataPort, value);
}

void flip(void*, uint32_t index) { write_reg(kRegYOffset, uint16_t(index * g_bochs.height)); }

void owner_changed(void*, uint32_t) {}

const display::Driver kDriver = {"bochs", nullptr, nullptr, owner_changed, flip, nullptr};

}  // namespace

void probe_bochs(uint32_t pci_index) {
    const pci::Device* p = pci::get(pci_index);
    const display::Device* current = display::get(0);
    if (!current || g_bochs.height) return;  // one card: the one that shows the console
    // DISPI registers: MMIO in BAR 2 when it is there (bochs-display, newer std-vga), else the VGA ports.
    if (p->bar_address[2] && p->bar_size[2] >= 0x600 && !(p->bar_flags[2] & pci::kBarIo)) {
        auto* regs = static_cast<uint8_t*>(pci::map_bar(pci_index, 2));
        if (regs) g_bochs.mmio = reinterpret_cast<volatile uint16_t*>(regs + 0x500);
    }
    uint16_t id = read_reg(kRegId);
    if (id < 0xB0C0 || id > 0xB0CF) return;
    uint32_t xres = read_reg(kRegXres), yres = read_reg(kRegYres), bpp = read_reg(kRegBpp);
    // Only take over what the console already shows: same mode, same memory (BAR 0).
    if (!(read_reg(kRegEnable) & 1) || xres != current->width || yres != current->height || bpp != 32 ||
        p->bar_address[0] != current->phys)
        return;
    uint64_t vram = uint64_t(read_reg(kRegVideoMemory64K)) * 65536;
    if (vram == 0) vram = p->bar_size[0];
    uint64_t frame = mem::align_up(uint64_t(current->pitch) * yres, pmm::kPageSize);
    // Buffer 1 must start a whole number of rows after buffer 0 (Y_OFFSET counts rows): page alignment of the
    // frame size is only a row multiple when pitch × height is already page aligned (true for usual modes).
    uint32_t buffers = vram >= 2 * frame && frame == uint64_t(current->pitch) * yres ? 2 : 1;
    if (buffers == 2) {
        write_reg(kRegVirtWidth, uint16_t(current->pitch / 4));
        write_reg(kRegVirtHeight, uint16_t(yres * 2));
        write_reg(kRegXOffset, 0);
        write_reg(kRegYOffset, 0);
        // QEMU derives the virtual height from the video memory itself (writes may be ignored): enough is enough.
        if (read_reg(kRegVirtHeight) < yres * 2) buffers = 1;
    }
    g_bochs.height = yres;
    display::Device d = *current;
    d.driver = &kDriver;
    d.context = &g_bochs;
    d.buffers = buffers;
    d.refresh_hz = 60;
    display::replace(0, d);
}

}  // namespace neko::drivers
