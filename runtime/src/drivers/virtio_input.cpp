// virtio-input: pointing devices QEMU attaches with `-device virtio-tablet-pci` (absolute: the guest
// cursor follows the host one, no mouse grab) or `-device virtio-mouse-pci` (relative).
//
// The device sends Linux evdev events (type, code, value) on queue 0; EV_SYN ends one report.
// Buffers of 8 bytes stay posted; whoever reaps them (MSI-X handler or a polling thread) turns
// them into neko::input events. Keyboards (virtio-keyboard-pci) are recognised but not used yet:
// the PS/2 and USB keyboards already cover it.
#include <neko/input.hpp>
#include <neko/process.hpp>

#include "virtio.hpp"

namespace neko::drivers {
namespace {

constexpr uint16_t kQueueSize = 64;

// Device configuration: write select/subsel, read size and the union behind it.
constexpr uint32_t kCfgSelect = 0, kCfgSubsel = 1, kCfgSize = 2, kCfgData = 8;
constexpr uint8_t kSelectName = 0x01, kSelectEvBits = 0x11, kSelectAbsInfo = 0x12;

// evdev
constexpr uint16_t kEvSyn = 0, kEvKey = 1, kEvRel = 2, kEvAbs = 3;
constexpr uint16_t kRelX = 0, kRelY = 1, kRelWheel = 8;
constexpr uint16_t kAbsX = 0, kAbsY = 1;
constexpr uint16_t kBtnLeft = 0x110, kBtnRight = 0x111, kBtnMiddle = 0x112;

struct InputEvent {
    uint16_t type, code;
    uint32_t value;
};

struct Pointer {
    virtio::Device dev;
    virtio::Queue events;
    InputEvent* buffers;  // DMA, one per descriptor
    int32_t min_x, max_x, min_y, max_y;
    // the report being assembled (until EV_SYN)
    int32_t dx, dy, wheel, ax, ay;
    bool moved_abs;
    uint8_t buttons;
    uint8_t reported;  // what input:: was last told
    char name[40];
};

void post(Pointer* p, uint16_t i) {
    p->events.desc[i] = {pmm::virt_to_phys(&p->buffers[i]), sizeof(InputEvent), virtio::kDescWrite, 0};
    virtio::publish(p->events, i);
}

int32_t scale(int32_t v, int32_t min, int32_t max) {
    if (max <= min) return 0;
    return int32_t((int64_t(v) - min) * 32767 / (int64_t(max) - min));
}

void handle(Pointer* p, const InputEvent& e) {
    auto value = int32_t(e.value);
    switch (e.type) {
        case kEvRel:
            if (e.code == kRelX) p->dx += value;
            else if (e.code == kRelY) p->dy += value;
            else if (e.code == kRelWheel) p->wheel -= value;  // evdev: positive = away from the user
            break;
        case kEvAbs:
            if (e.code == kAbsX) p->ax = value, p->moved_abs = true;
            else if (e.code == kAbsY) p->ay = value, p->moved_abs = true;
            break;
        case kEvKey: {
            uint8_t bit = e.code == kBtnLeft ? 1 : e.code == kBtnRight ? 2 : e.code == kBtnMiddle ? 4 : 0;
            if (bit) p->buttons = value ? uint8_t(p->buttons | bit) : uint8_t(p->buttons & ~bit);
            break;
        }
        case kEvSyn:
            if (p->moved_abs) input::absolute(scale(p->ax, p->min_x, p->max_x), scale(p->ay, p->min_y, p->max_y));
            input::motion(p->dx, p->dy);
            input::buttons(p->reported, p->buttons);
            input::wheel(p->wheel);
            p->dx = p->dy = p->wheel = 0;
            p->moved_abs = false;
            break;
    }
}

void reap(Pointer* p) {
    sync::ScopedLock guard(p->events.lock);
    uint32_t head, length;
    while (virtio::next_used(p->events, head, length)) {
        if (head >= p->events.size) continue;
        handle(p, p->buffers[head]);
        post(p, uint16_t(head));
    }
}

void on_interrupt(void* context) { reap(static_cast<Pointer*>(context)); }

void poll_thread(uint64_t context) {
    for (;;) {
        reap(reinterpret_cast<Pointer*>(context));
        sched::sleep_ms(2);
    }
}

// Selects a configuration item; returns its size (0: the device has nothing there).
uint8_t select(virtio::Device& d, uint8_t sel, uint8_t subsel) {
    d.config[kCfgSelect] = sel;
    d.config[kCfgSubsel] = subsel;
    return virtio::config8(d, kCfgSize);
}

bool has_bit(virtio::Device& d, uint8_t ev_type, uint16_t code) {
    uint8_t size = select(d, kSelectEvBits, ev_type);
    if (code / 8 >= size) return false;
    return (virtio::config8(d, kCfgData + code / 8) >> (code % 8)) & 1;
}

void abs_range(virtio::Device& d, uint8_t axis, int32_t& min, int32_t& max) {
    min = 0, max = 32767;
    if (select(d, kSelectAbsInfo, axis) < 8) return;
    min = int32_t(virtio::config32(d, kCfgData));
    max = int32_t(virtio::config32(d, kCfgData + 4));
}

}  // namespace

void probe_virtio_input(uint32_t pci) {
    auto* p = static_cast<Pointer*>(heap::alloc_zeroed(sizeof(Pointer)));
    if (!p) return;
    if (!virtio::attach(p->dev, pci)) return virtio::fail(p->dev, "virtio-input", "no virtio 1.0 capabilities");
    if (!virtio::negotiate(p->dev, 0, 0)) return virtio::fail(p->dev, "virtio-input", "features not accepted");

    uint8_t n = select(p->dev, kSelectName, 0);
    for (uint8_t i = 0; i < n && i < sizeof(p->name) - 1; ++i) p->name[i] = char(virtio::config8(p->dev, kCfgData + i));
    bool absolute = has_bit(p->dev, kEvAbs, kAbsX);
    bool relative = has_bit(p->dev, kEvRel, kRelX);
    if (!absolute && !relative) {
        log::info("virtio-input: a keyboard (not used: PS/2 and USB keyboards are)");
        return virtio::fail(p->dev, "virtio-input", "not a pointing device");
    }
    if (absolute) {
        abs_range(p->dev, kAbsX, p->min_x, p->max_x);
        abs_range(p->dev, kAbsY, p->min_y, p->max_y);
    }

    uint8_t vector = p->dev.msix ? irq::allocate_msi(&on_interrupt, p, "virtio-input") : 0;
    if (!virtio::setup_queue(p->dev, p->events, 0, kQueueSize, vector, 0))
        return virtio::fail(p->dev, "virtio-input", "no event queue");
    p->buffers = static_cast<InputEvent*>(pmm::alloc_page());  // 64 × 8 bytes
    if (!p->buffers) return virtio::fail(p->dev, "virtio-input", "out of memory");
    {
        sync::ScopedLock guard(p->events.lock);
        for (uint16_t i = 0; i < p->events.size; ++i) post(p, i);
    }
    virtio::driver_ok(p->dev);
    if (!p->events.interrupts) sched::spawn("virtio-input poll", &poll_thread, reinterpret_cast<uint64_t>(p), sched::Priority::High);

    term::write("[neko] virtio-input: ");
    term::write(p->name[0] ? p->name : "pointer");
    term::write(absolute ? " (absolute)" : " (relative)");
    term::write(p->events.interrupts ? ", MSI-X\n" : ", polling\n");
}

}  // namespace neko::drivers
