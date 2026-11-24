// xHCI (USB 3 host controller; QEMU: -device qemu-xhci) and HID boot keyboards / mice.
//
//   capability registers → operational registers (USBCMD, USBSTS, CRCR, DCBAAP, CONFIG, PORTSC…)
//   → runtime registers (interrupter 0: event ring) → doorbells (0 = commands, n = device slot n)
//
// Rings are pages of 16-byte TRBs whose last entry links back to the start; the cycle bit tells the
// consumer which entries are new. Start-up: take the controller from the BIOS, reset it, set up the
// device context array (+ scratchpad pages), the command ring and the event ring, run. Then every
// connected root port is reset and its device enumerated: Enable Slot → Address Device → device and
// configuration descriptors → SET_CONFIGURATION; boot-protocol HID interfaces get SET_PROTOCOL(boot),
// SET_IDLE and their interrupt-IN endpoint configured with one transfer always queued.
//
// The event ring is drained in the MSI-X handler: command and control-transfer completions wake the
// thread waiting for them; HID reports are decoded there (keyboard → PS/2 scancodes for the C#
// keyboard driver, which runs in interrupt context too) and the transfer is queued again.
#include <neko/input.hpp>
#include <neko/process.hpp>
#include <neko/runtime.hpp>
#include <neko/usb.hpp>

namespace neko::usb {
namespace {

using namespace proc;  // error codes

// ---- registers
constexpr uint32_t kCapLength = 0x00, kHcsParams1 = 0x04, kHcsParams2 = 0x08, kHccParams1 = 0x10, kDbOff = 0x14, kRtsOff = 0x18;
constexpr uint32_t kUsbCmd = 0x00, kUsbSts = 0x04, kCrcr = 0x18, kDcbaap = 0x30, kConfig = 0x38, kPortBase = 0x400;
constexpr uint32_t kCmdRun = 1, kCmdReset = 2, kCmdInte = 4;
constexpr uint32_t kStsHalted = 1, kStsEint = 8, kStsCnr = 1u << 11;
constexpr uint32_t kPortCcs = 1, kPortPed = 2, kPortPr = 1u << 4, kPortPrc = 1u << 21;
constexpr uint32_t kPortChangeBits = 0x00FE0000;  // RW1C status-change bits 17–23

// ---- TRB types
constexpr uint32_t kTrbNormal = 1, kTrbSetup = 2, kTrbData = 3, kTrbStatus = 4, kTrbLink = 6;
constexpr uint32_t kTrbEnableSlot = 9, kTrbAddressDevice = 11, kTrbConfigureEndpoint = 12, kTrbEvaluateContext = 13;
constexpr uint32_t kTrbTransferEvent = 32, kTrbCommandCompletion = 33, kTrbPortStatusChange = 34;
constexpr uint32_t kIoc = 1u << 5, kIdt = 1u << 6;
constexpr uint32_t kSuccess = 1, kShortPacket = 13;

constexpr uint32_t kRingTrbs = 256;  // one page
constexpr uint32_t kMaxSlots = 16;

struct Trb {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
};

struct Ring {
    Trb* trbs;
    uint64_t phys;
    uint32_t index;
    uint32_t cycle;
};

bool make_ring(Ring& r) {
    r.trbs = static_cast<Trb*>(pmm::alloc_page());
    if (!r.trbs) return false;
    r.phys = pmm::virt_to_phys(r.trbs);
    r.index = 0;
    r.cycle = 1;
    Trb& link = r.trbs[kRingTrbs - 1];  // back to the start, toggling the cycle state
    link.parameter = r.phys;
    link.control = (kTrbLink << 10) | 2;
    return true;
}

// Queues a TRB (the cycle bit is set here). Returns its physical address.
uint64_t push(Ring& r, uint64_t parameter, uint32_t status, uint32_t control) {
    Trb& t = r.trbs[r.index];
    uint64_t address = r.phys + r.index * sizeof(Trb);
    t.parameter = parameter;
    t.status = status;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    t.control = (control & ~1u) | r.cycle;
    if (++r.index == kRingTrbs - 1) {
        Trb& link = r.trbs[kRingTrbs - 1];
        link.control = (link.control & ~1u) | r.cycle;
        r.index = 0;
        r.cycle ^= 1;
    }
    return address;
}

// ---- devices

constexpr uint8_t kKindOther = 0, kKindKeyboard = 1, kKindMouse = 2;

struct Device {
    Info info;
    uint32_t slot;
    void* output;      // device context (owned by the controller)
    void* input;       // input context (ours)
    Ring ep0;
    uint8_t* buffer;   // one page for control transfers
    // HID interrupt endpoint
    uint32_t hid_dci;
    Ring hid_ring;
    uint8_t* report;   // DMA
    uint32_t report_size;
    uint8_t interface;
    uint8_t last[8];   // previous keyboard report
    uint8_t held;      // keyboard: the last key pressed (for repeat)
    uint64_t held_since, repeated_at;
};

struct Controller {
    volatile uint8_t* cap;
    volatile uint8_t* op;
    volatile uint8_t* rt;
    volatile uint32_t* doorbells;
    uint32_t context_size;  // 32 or 64
    uint32_t ports;
    uint64_t* dcbaa;
    Ring commands;
    Trb* events;
    uint64_t events_phys;
    uint32_t event_index, event_cycle;
    Device* slots[kMaxSlots + 1];
    bool interrupts;

    // the one command / control transfer in flight (start-up is sequential)
    sync::Semaphore command_done, transfer_done;
    volatile uint32_t command_code, command_slot;
    volatile uint32_t transfer_code, transfer_residue;
    sync::SpinLock lock;
};

Controller* g_hc = nullptr;
Device* g_devices[kMaxSlots];
uint32_t g_device_count = 0;
ScancodeSink g_sink = nullptr;
Mouse g_mouse;

uint32_t r32(volatile uint8_t* b, uint32_t o) { return *reinterpret_cast<volatile uint32_t*>(b + o); }
void w32(volatile uint8_t* b, uint32_t o, uint32_t v) { *reinterpret_cast<volatile uint32_t*>(b + o) = v; }
void w64(volatile uint8_t* b, uint32_t o, uint64_t v) {
    w32(b, o, uint32_t(v));
    w32(b, o + 4, uint32_t(v >> 32));
}

uint32_t* context(Controller* c, void* base, uint32_t index) {
    return reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(base) + index * c->context_size);
}

// ---- keyboard: HID usage → PS/2 set 1 (0xE0xx = extended)

const uint16_t kUsageToSet1[0x66] = {
    0, 0, 0, 0, 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26,              // 00–0F: a…l
    0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C, 0x02, 0x03,  // m…z, 1, 2
    0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x1C, 0x01, 0x0E, 0x0F, 0x39, 0x0C, 0x0D, 0x1A,  // 3…0, Enter, Esc, BS, Tab, Space, - = [
    0x1B, 0x2B, 0x2B, 0x27, 0x28, 0x29, 0x33, 0x34, 0x35, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40,  // ] \ # ; ' ` , . / Caps F1…F6
    0x41, 0x42, 0x43, 0x44, 0x57, 0x58, 0xE037, 0x46, 0, 0xE052, 0xE047, 0xE049, 0xE053, 0xE04F, 0xE051, 0xE04D,  // F7…F12 PrtSc ScrLk Pause Ins Home PgUp Del End PgDn Right
    0xE04B, 0xE050, 0xE048, 0x45, 0xE035, 0x37, 0x4A, 0x4E, 0xE01C, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47,  // Left Down Up NumLk KP/ KP* KP- KP+ KPEnter KP1…KP7
    0x48, 0x49, 0x52, 0x53, 0x56, 0xE05D,                                                            // KP8 KP9 KP0 KP. \| App
};
const uint16_t kModifierSet1[8] = {0x1D, 0x2A, 0x38, 0xE05B, 0xE01D, 0x36, 0xE038, 0xE05C};

void emit(uint16_t code, bool released) {
    if (!code || !g_sink) return;
    if (code & 0xFF00) g_sink(0xE0);
    g_sink(uint8_t((code & 0x7F) | (released ? 0x80 : 0)));
}

bool contains(const uint8_t* keys, uint8_t usage) {
    for (int i = 2; i < 8; ++i)
        if (keys[i] == usage) return true;
    return false;
}

void keyboard_report(Device* d, const uint8_t* r) {
    if (r[2] == 1) return;  // phantom state (too many keys)
    uint8_t changed = uint8_t(r[0] ^ d->last[0]);
    for (int b = 0; b < 8; ++b)
        if (changed & (1u << b)) emit(kModifierSet1[b], !(r[0] & (1u << b)));
    for (int i = 2; i < 8; ++i)  // released
        if (d->last[i] && !contains(r, d->last[i]) && d->last[i] < 0x66) emit(kUsageToSet1[d->last[i]], true);
    uint64_t now = time::uptime_ms();
    bool any_new = false;
    for (int i = 2; i < 8; ++i) {  // pressed
        if (r[i] && !contains(d->last, r[i]) && r[i] < 0x66) {
            emit(kUsageToSet1[r[i]], false);
            d->held = r[i];
            d->held_since = now;
            any_new = true;
        }
    }
    // Typematic repeat: the device resends its report every 32 ms (SET_IDLE); a key held for half a
    // second repeats about 30 times a second.
    if (!any_new && d->held && contains(r, d->held) && now - d->held_since >= 500 && now - d->repeated_at >= 30) {
        emit(kUsageToSet1[d->held], false);
        d->repeated_at = now;
    }
    if (d->held && !contains(r, d->held)) d->held = 0;
    mem::copy(d->last, r, 8);
}

void mouse_report(const uint8_t* r, uint32_t length) {
    g_mouse.buttons = r[0] & 7;
    if (length >= 3) {
        g_mouse.x += int8_t(r[1]);
        g_mouse.y += int8_t(r[2]);
        input::motion(int8_t(r[1]), int8_t(r[2]));
    }
    static uint8_t last = 0;  // all USB mice together count as one device
    input::buttons(last, r[0] & 7);
    if (length >= 4) {
        g_mouse.wheel += int8_t(r[3]);
        input::wheel(-int8_t(r[3]));  // HID: positive = away from the user; events: positive = scroll down
    }
    ++g_mouse.reports;
}

void queue_report(Controller* c, Device* d) {
    push(d->hid_ring, pmm::virt_to_phys(d->report), d->report_size, (kTrbNormal << 10) | kIoc);
    c->doorbells[d->slot] = d->hid_dci;
}

// ---- events (interrupt handler, or polling)

void handle_events(Controller* c) {
    sync::ScopedLock guard(c->lock);
    bool any = false;
    for (;;) {
        Trb& e = c->events[c->event_index];
        if ((e.control & 1) != c->event_cycle) break;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        uint32_t type = (e.control >> 10) & 0x3F;
        uint32_t code = e.status >> 24;
        uint32_t slot = e.control >> 24;
        if (type == kTrbCommandCompletion) {
            c->command_code = code;
            c->command_slot = slot;
            c->command_done.signal();
        } else if (type == kTrbTransferEvent) {
            uint32_t dci = (e.control >> 16) & 0x1F;
            Device* d = slot <= kMaxSlots ? c->slots[slot] : nullptr;
            if (dci == 1) {
                c->transfer_code = code;
                c->transfer_residue = e.status & 0xFFFFFF;
                c->transfer_done.signal();
            } else if (d && dci == d->hid_dci) {
                if (code == kSuccess || code == kShortPacket) {
                    uint32_t got = d->report_size - (e.status & 0xFFFFFF);
                    ++d->info.reports;
                    if (d->info.kind == kKindKeyboard && got >= 8) keyboard_report(d, d->report);
                    else if (d->info.kind == kKindMouse && got >= 3) mouse_report(d->report, got);
                }
                queue_report(c, d);
            }
        } else if (type == kTrbPortStatusChange) {
            uint32_t port = uint32_t(e.parameter >> 24) & 0xFF;
            if (port >= 1 && port <= c->ports) {  // acknowledge (hot-plug is not handled yet)
                uint32_t off = kPortBase + 0x10 * (port - 1);
                uint32_t v = r32(c->op, off);
                w32(c->op, off, (v & ~(kPortPed | kPortChangeBits)) | (v & kPortChangeBits));
            }
        }
        if (++c->event_index == kRingTrbs) {
            c->event_index = 0;
            c->event_cycle ^= 1;
        }
        any = true;
    }
    if (any) {  // tell the controller how far we got (EHB cleared by writing 1)
        volatile uint8_t* ir = c->rt + 0x20;
        w64(ir, 0x18, (c->events_phys + c->event_index * sizeof(Trb)) | 8);
    }
}

void on_interrupt(void* context) {
    auto* c = static_cast<Controller*>(context);
    w32(c->op, kUsbSts, kStsEint);
    volatile uint8_t* ir = c->rt + 0x20;
    w32(ir, 0, r32(ir, 0) | 1);  // IMAN.IP (RW1C), IE stays set
    handle_events(c);
}

void poll_thread(uint64_t context) {
    auto* c = reinterpret_cast<Controller*>(context);
    for (;;) {
        {
            cpu::InterruptGuard g;  // like an interrupt handler (the keyboard driver expects that)
            handle_events(c);
        }
        sched::sleep_ms(2);
    }
}

// Waits for `done` (or polls), up to `timeout_ms`.
bool wait(Controller* c, sync::Semaphore& done, uint64_t timeout_ms) {
    uint64_t until = time::uptime_ms() + timeout_ms;
    for (;;) {
        if (done.try_wait()) return true;
        if (!c->interrupts) handle_events(c);
        if (time::uptime_ms() > until) return false;
        sched::sleep_ms(1);
    }
}

// A command; its completion code (and the slot id for Enable Slot).
uint32_t command(Controller* c, uint64_t parameter, uint32_t control, uint32_t* slot = nullptr) {
    while (c->command_done.try_wait()) {
    }
    {
        sync::ScopedLock guard(c->lock);
        push(c->commands, parameter, 0, control);
        c->doorbells[0] = 0;
    }
    if (!wait(c, c->command_done, 1000)) return 0;
    if (slot) *slot = c->command_slot;
    return c->command_code;
}

// Control transfer on EP0; `length` bytes in or out of d->buffer. 0 or an Error.
int64_t control(Controller* c, Device* d, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
                uint16_t length) {
    while (c->transfer_done.try_wait()) {
    }
    bool in = request_type & 0x80;
    uint64_t setup = uint64_t(request_type) | (uint64_t(request) << 8) | (uint64_t(value) << 16) |
                     (uint64_t(index) << 32) | (uint64_t(length) << 48);
    uint32_t trt = length == 0 ? 0 : in ? 3 : 2;
    {
        sync::ScopedLock guard(c->lock);
        push(d->ep0, setup, 8, (kTrbSetup << 10) | kIdt | (trt << 16));
        if (length) push(d->ep0, pmm::virt_to_phys(d->buffer), length, (kTrbData << 10) | (in ? 1u << 16 : 0));
        push(d->ep0, 0, 0, (kTrbStatus << 10) | kIoc | ((length == 0 || !in) ? 1u << 16 : 0));
        c->doorbells[d->slot] = 1;
    }
    if (!wait(c, c->transfer_done, 1000)) return kErrIo;
    return c->transfer_code == kSuccess || c->transfer_code == kShortPacket ? 0 : kErrIo;
}

void read_string(Controller* c, Device* d, uint8_t index, uint8_t* out, uint32_t capacity) {
    out[0] = 0;
    if (!index || control(c, d, 0x80, 6, uint16_t(0x0300 | index), 0x0409, 255) < 0) return;
    uint32_t length = d->buffer[0], n = 0;
    for (uint32_t i = 2; i + 1 < length && n + 1 < capacity; i += 2) {
        uint16_t ch = uint16_t(d->buffer[i] | (d->buffer[i + 1] << 8));
        out[n++] = ch < 0x80 ? uint8_t(ch) : '?';
    }
    while (n > 0 && out[n - 1] == ' ') --n;
    out[n] = 0;
}

uint32_t ep0_packet(uint32_t speed) { return speed == 4 ? 512 : speed == 3 ? 64 : 8; }

// Interval field of an interrupt endpoint context from bInterval.
uint32_t interval(uint32_t speed, uint8_t b) {
    if (speed >= 3) return b ? (b - 1u > 15 ? 15u : b - 1u) : 0;  // 2^(bInterval-1) × 125 µs
    uint32_t frames = b ? b : 1, e = 3;                          // ms → 125 µs exponent
    while ((1u << (e + 1)) <= frames * 8 && e < 10) ++e;
    return e;
}

void enumerate(Controller* c, uint32_t port, uint32_t speed) {
    uint32_t slot = 0;
    if (command(c, 0, kTrbEnableSlot << 10, &slot) != kSuccess || slot == 0 || slot > kMaxSlots) return;
    auto* d = static_cast<Device*>(heap::alloc_zeroed(sizeof(Device)));
    if (!d) return;
    d->slot = slot;
    d->info.slot = uint8_t(slot);
    d->info.port = uint8_t(port);
    d->info.speed = uint8_t(speed);
    d->output = pmm::alloc_page();
    d->input = pmm::alloc_page();
    d->buffer = static_cast<uint8_t*>(pmm::alloc_page());
    if (!d->output || !d->input || !d->buffer || !make_ring(d->ep0)) return;
    c->dcbaa[slot] = pmm::virt_to_phys(d->output);
    c->slots[slot] = d;

    // Address Device: slot context + EP0.
    uint32_t* icc = context(c, d->input, 0);
    icc[1] = 1 | 2;  // add slot, EP0
    uint32_t* sc = context(c, d->input, 1);
    sc[0] = (speed << 20) | (1u << 27);
    sc[1] = port << 16;
    uint32_t* ep = context(c, d->input, 2);
    ep[1] = (3u << 1) | (4u << 3) | (ep0_packet(speed) << 16);  // CErr 3, control, max packet
    ep[2] = uint32_t(d->ep0.phys) | 1;
    ep[3] = uint32_t(d->ep0.phys >> 32);
    ep[4] = 8;
    if (command(c, pmm::virt_to_phys(d->input), (kTrbAddressDevice << 10) | (slot << 24)) != kSuccess) {
        term::write("[neko] usb: port ");
        term::write_udec(port);
        term::write(": Address Device failed\n");
        return;
    }

    // Device descriptor: the first 8 bytes tell EP0's real packet size (full speed may be 8–64).
    if (control(c, d, 0x80, 6, 0x0100, 0, 8) < 0) return;
    uint32_t packet = d->buffer[7];
    if (speed == 1 && packet != ep0_packet(speed) && packet >= 8) {
        mem::set(d->input, 0, 4096);
        icc[1] = 2;
        ep[1] = (3u << 1) | (4u << 3) | (packet << 16);
        ep[2] = uint32_t(d->ep0.phys) | 1;
        ep[3] = uint32_t(d->ep0.phys >> 32);
        ep[4] = 8;
        command(c, pmm::virt_to_phys(d->input), (kTrbEvaluateContext << 10) | (slot << 24));
    }
    if (control(c, d, 0x80, 6, 0x0100, 0, 18) < 0) return;
    d->info.vendor_id = uint16_t(d->buffer[8] | (d->buffer[9] << 8));
    d->info.product_id = uint16_t(d->buffer[10] | (d->buffer[11] << 8));
    d->info.class_code = d->buffer[4];
    d->info.subclass = d->buffer[5];
    d->info.protocol = d->buffer[6];
    uint8_t product_index = d->buffer[15];
    read_string(c, d, product_index, d->info.product, sizeof(d->info.product));

    // Configuration 1 with its interfaces and endpoints.
    if (control(c, d, 0x80, 6, 0x0200, 0, 9) < 0) return;
    uint16_t total = uint16_t(d->buffer[2] | (d->buffer[3] << 8));
    if (total > 4096) total = 4096;
    if (control(c, d, 0x80, 6, 0x0200, 0, total) < 0) return;
    uint8_t configuration = d->buffer[5];
    uint8_t kind = kKindOther, iface = 0, ep_address = 0, ep_interval = 0;
    uint16_t ep_packet = 0;
    bool in_hid = false;
    for (uint32_t at = 0; at + 2 <= total && d->buffer[at] >= 2; at += d->buffer[at]) {
        const uint8_t* desc = d->buffer + at;
        if (desc[1] == 4) {  // interface: HID boot keyboard (3/1/1) or mouse (3/1/2)
            in_hid = kind == kKindOther && desc[5] == 3 && desc[6] == 1 && (desc[7] == 1 || desc[7] == 2);
            if (in_hid) {
                kind = desc[7] == 1 ? kKindKeyboard : kKindMouse;
                iface = desc[2];
                d->info.class_code = 3;
                d->info.subclass = 1;
                d->info.protocol = desc[7];
            }
        } else if (desc[1] == 5 && in_hid && !ep_address && (desc[2] & 0x80) && (desc[3] & 3) == 3) {
            ep_address = desc[2];
            ep_packet = uint16_t((desc[4] | (desc[5] << 8)) & 0x7FF);
            ep_interval = desc[6];
        }
    }
    if (control(c, d, 0x00, 9, configuration, 0, 0) < 0) return;  // SET_CONFIGURATION
    g_devices[g_device_count++] = d;
    if (!ep_address) return;

    // HID: boot protocol, a report every 32 ms (repeat), and the interrupt-IN endpoint.
    control(c, d, 0x21, 0x0B, 0, iface, 0);       // SET_PROTOCOL(boot)
    control(c, d, 0x21, 0x0A, 8 << 8, iface, 0);  // SET_IDLE(32 ms)
    d->interface = iface;
    d->hid_dci = (ep_address & 0xF) * 2 + 1;
    d->report_size = ep_packet < 8 ? 8 : (ep_packet > 64 ? 64 : ep_packet);
    d->report = static_cast<uint8_t*>(pmm::alloc_page());
    if (!d->report || !make_ring(d->hid_ring)) return;
    mem::set(d->input, 0, 4096);
    icc[1] = 1 | (1u << d->hid_dci);
    mem::copy(sc, context(c, d->output, 0), c->context_size);  // the slot as the controller has it
    sc[0] = (sc[0] & ~(0x1Fu << 27)) | (d->hid_dci << 27);
    uint32_t* hep = context(c, d->input, d->hid_dci + 1);
    hep[0] = interval(speed, ep_interval) << 16;
    hep[1] = (3u << 1) | (7u << 3) | (uint32_t(ep_packet) << 16);  // CErr 3, interrupt IN
    hep[2] = uint32_t(d->hid_ring.phys) | 1;
    hep[3] = uint32_t(d->hid_ring.phys >> 32);
    hep[4] = d->report_size | (uint32_t(ep_packet) << 16);
    if (command(c, pmm::virt_to_phys(d->input), (kTrbConfigureEndpoint << 10) | (slot << 24)) != kSuccess) {
        term::write("[neko] usb: Configure Endpoint failed\n");
        return;
    }
    d->info.kind = kind;
    sync::ScopedLock guard(c->lock);
    queue_report(c, d);
}

void reset_port(Controller* c, uint32_t port) {
    uint32_t off = kPortBase + 0x10 * (port - 1);
    uint32_t v = r32(c->op, off);
    if (!(v & kPortCcs)) return;
    if (!(v & kPortPed)) {  // USB 2 ports need a reset to be enabled; USB 3 ones train by themselves
        w32(c->op, off, (v & ~(kPortPed | kPortChangeBits)) | kPortPr);
        uint64_t until = time::uptime_ms() + 500;
        while (!(r32(c->op, off) & kPortPrc) && time::uptime_ms() < until) sched::sleep_ms(1);
        v = r32(c->op, off);
        w32(c->op, off, (v & ~(kPortPed | kPortChangeBits)) | (v & kPortChangeBits));  // acknowledge
        sched::sleep_ms(10);
        v = r32(c->op, off);
    }
    if (!(v & kPortPed)) return;
    enumerate(c, port, (v >> 10) & 0xF);
}

void take_from_bios(Controller* c) {
    uint32_t offset = (r32(c->cap, kHccParams1) >> 16) << 2;
    for (int guard = 0; offset && guard < 64; ++guard) {
        volatile uint8_t* x = c->cap + offset;
        uint32_t v = r32(x, 0);
        if ((v & 0xFF) == 1) {  // USB legacy support: claim the OS-owned semaphore
            w32(x, 0, v | (1u << 24));
            uint64_t until = time::uptime_ms() + 1000;
            while ((r32(x, 0) & (1u << 16)) && time::uptime_ms() < until) sched::sleep_ms(1);
            w32(x, 4, r32(x, 4) & ~0x1F) ;  // no more SMIs for USB events
        }
        uint32_t next = (v >> 8) & 0xFF;
        offset = next ? offset + (next << 2) : 0;
    }
}

}  // namespace

uint32_t count() { return g_device_count; }

bool info(uint32_t index, Info& out) {
    if (index >= g_device_count) return false;
    out = g_devices[index]->info;
    return true;
}

void set_keyboard_sink(ScancodeSink sink) { __atomic_store_n(&g_sink, sink, __ATOMIC_RELEASE); }

Mouse mouse() { return g_mouse; }

}  // namespace neko::usb

namespace neko::drivers {

void probe_xhci(uint32_t pci) {
    using namespace neko::usb;
    if (g_hc) return;  // one controller for now
    auto* base = static_cast<volatile uint8_t*>(pci::map_bar(pci, 0));
    if (!base) return;
    auto* c = static_cast<Controller*>(heap::alloc_zeroed(sizeof(Controller)));
    if (!c) return;
    pci::enable(pci, true, false, true);
    c->cap = base;
    c->op = base + (r32(base, kCapLength) & 0xFF);
    c->rt = base + (r32(base, kRtsOff) & ~0x1Fu);
    c->doorbells = reinterpret_cast<volatile uint32_t*>(base + (r32(base, kDbOff) & ~0x3u));
    uint32_t hcs1 = r32(base, kHcsParams1), hcs2 = r32(base, kHcsParams2), hcc1 = r32(base, kHccParams1);
    c->ports = hcs1 >> 24;
    c->context_size = (hcc1 & 4) ? 64 : 32;
    uint32_t slots = (hcs1 & 0xFF) < kMaxSlots ? (hcs1 & 0xFF) : kMaxSlots;
    if (!(hcc1 & 1)) {
        term::write("[neko] xhci: no 64-bit addressing — ignored\n");
        return;
    }
    take_from_bios(c);

    // Stop, reset.
    w32(c->op, kUsbCmd, r32(c->op, kUsbCmd) & ~kCmdRun);
    uint64_t until = time::uptime_ms() + 100;
    while (!(r32(c->op, kUsbSts) & kStsHalted) && time::uptime_ms() < until) sched::sleep_ms(1);
    w32(c->op, kUsbCmd, kCmdReset);
    until = time::uptime_ms() + 1000;
    while ((r32(c->op, kUsbCmd) & kCmdReset) || (r32(c->op, kUsbSts) & kStsCnr)) {
        if (time::uptime_ms() > until) {
            term::write("[neko] xhci: reset does not finish — ignored\n");
            return;
        }
        sched::sleep_ms(1);
    }

    // Device context array (+ scratchpad buffers), command ring, event ring.
    w32(c->op, kConfig, slots);
    c->dcbaa = static_cast<uint64_t*>(pmm::alloc_page());
    if (!c->dcbaa || !make_ring(c->commands)) return;
    uint32_t scratch = ((hcs2 >> 21) & 0x1F) << 5 | ((hcs2 >> 27) & 0x1F);
    if (scratch) {
        auto* array = static_cast<uint64_t*>(pmm::alloc_page());
        if (!array) return;
        for (uint32_t i = 0; i < scratch && i < 512; ++i) {
            void* page = pmm::alloc_page();
            if (!page) return;
            array[i] = pmm::virt_to_phys(page);
        }
        c->dcbaa[0] = pmm::virt_to_phys(array);
    }
    w64(c->op, kDcbaap, pmm::virt_to_phys(c->dcbaa));
    w64(c->op, kCrcr, c->commands.phys | 1);
    c->events = static_cast<Trb*>(pmm::alloc_page());
    auto* erst = static_cast<uint64_t*>(pmm::alloc_page());  // one segment
    if (!c->events || !erst) return;
    c->events_phys = pmm::virt_to_phys(c->events);
    c->event_cycle = 1;
    erst[0] = c->events_phys;
    erst[1] = kRingTrbs;
    volatile uint8_t* ir = c->rt + 0x20;
    w32(ir, 0x08, 1);  // ERSTSZ
    w64(ir, 0x18, c->events_phys);
    w64(ir, 0x10, pmm::virt_to_phys(erst));

    // Interrupter 0 by MSI-X or MSI; otherwise a thread polls the event ring.
    uint8_t vector = irq::allocate_msi(&on_interrupt, c, "xhci");
    if (vector) {
        if (pci::msix_enable(pci) > 0) c->interrupts = pci::msix_route(pci, 0, vector);
        else c->interrupts = pci::msi_enable(pci, vector);
    }
    if (c->interrupts) {
        w32(ir, 0x04, 4000);  // IMOD: at most one interrupt per ms (250 ns units)
        w32(ir, 0, 2 | 1);    // IMAN: enable, clear pending
    }
    g_hc = c;
    w32(c->op, kUsbCmd, kCmdRun | (c->interrupts ? kCmdInte : 0));
    until = time::uptime_ms() + 100;
    while ((r32(c->op, kUsbSts) & kStsHalted) && time::uptime_ms() < until) sched::sleep_ms(1);
    sched::sleep_ms(50);  // ports report their connections

    for (uint32_t p = 1; p <= c->ports; ++p) reset_port(c, p);
    if (!c->interrupts) sched::spawn("xhci poll", &poll_thread, reinterpret_cast<uint64_t>(c), sched::Priority::High);

    term::write("[neko] xhci: ");
    term::write_udec(c->ports);
    term::write(" ports, ");
    term::write_udec(g_device_count);
    term::write(" device(s)");
    for (uint32_t i = 0; i < g_device_count; ++i) {
        term::write(i ? ", " : ": ");
        const Info& d = g_devices[i]->info;
        term::write(d.kind == kKindKeyboard ? "keyboard" : d.kind == kKindMouse ? "mouse" : "device");
        if (d.product[0]) {
            term::write(" \"");
            term::write(d.product);
            term::write("\"");
        }
    }
    term::write(c->interrupts ? ", interrupts\n" : ", polling\n");
}

}  // namespace neko::drivers
