// xHCI (USB 3 host controller; QEMU: -device qemu-xhci), hubs, hot plug, HID boot keyboards / mice and
// mass storage.
//
//   capability registers → operational registers (USBCMD, USBSTS, CRCR, DCBAAP, CONFIG, PORTSC…)
//   → runtime registers (interrupter 0: event ring) → doorbells (0 = commands, n = device slot n)
//
// Rings are pages of 16-byte TRBs whose last entry links back to the start; the cycle bit tells the
// consumer which entries are new. Start-up: take the controller from the BIOS, reset it, set up the device
// context array (+ scratchpad pages), the command ring and the event ring, run.
//
// Topology and hot plug: one thread ("usb-hub") owns every change of the device tree. Root ports report
// changes with Port Status Change events, hubs with their status-change interrupt endpoint; the interrupt
// handler only records which ports changed and wakes the thread. The thread reads the port, and when a device
// arrived resets the port and enumerates it (Enable Slot → Address Device with its route string / TT →
// descriptors → SET_CONFIGURATION → class driver), when one left detaches it (children first, Disable Slot).
// At boot the probe waits until the first scan of the whole tree is done, so the keyboard is there before
// the shell starts.
//
// Class drivers:
//   HID boot keyboard / mouse   SET_PROTOCOL(boot), SET_IDLE, an interrupt-IN transfer always queued; reports
//                               are decoded in the interrupt handler (keyboard → PS/2 set 1 for the C# driver)
//   hub (class 9)               hub descriptor, Configure Endpoint with the Hub bit / ports / TT think time,
//                               SET_HUB_DEPTH (USB 3), port power, the status-change endpoint
//   mass storage (class 8/6/80) Bulk-Only Transport: CBW → data → CSW, SCSI INQUIRY / TEST UNIT READY /
//                               READ CAPACITY(10) / READ(10) / WRITE(10) / SYNCHRONIZE CACHE, stalls cleared
//                               (Reset Endpoint + Set TR Dequeue + CLEAR_FEATURE), BOT reset recovery;
//                               registered as block device usbN (block::add), removed on unplug
#include <neko/block.hpp>
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
constexpr uint32_t kPortCcs = 1, kPortPed = 2, kPortPr = 1u << 4, kPortCsc = 1u << 17, kPortPrc = 1u << 21;
constexpr uint32_t kPortChangeBits = 0x00FE0000;  // RW1C status-change bits 17–23

// ---- TRB types
constexpr uint32_t kTrbNormal = 1, kTrbSetup = 2, kTrbData = 3, kTrbStatus = 4, kTrbLink = 6;
constexpr uint32_t kTrbEnableSlot = 9, kTrbDisableSlot = 10, kTrbAddressDevice = 11, kTrbConfigureEndpoint = 12,
                   kTrbEvaluateContext = 13, kTrbResetEndpoint = 14, kTrbSetDequeue = 16;
constexpr uint32_t kTrbTransferEvent = 32, kTrbCommandCompletion = 33, kTrbPortStatusChange = 34;
constexpr uint32_t kIoc = 1u << 5, kIdt = 1u << 6, kIsp = 1u << 2;
constexpr uint32_t kSuccess = 1, kStall = 6, kShortPacket = 13;

constexpr uint32_t kRingTrbs = 256;  // one page
constexpr uint32_t kMaxSlots = 32;
constexpr uint32_t kMaxPorts = 32;   // root ports we look after
constexpr uint32_t kMaxHubPorts = 15;
constexpr uint32_t kStorageBuffer = 65536;  // one bulk transfer (aligned: never crosses a 64 KiB boundary)

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

struct Endpoint {
    Ring ring;
    sync::Semaphore done;
    volatile uint32_t code, residue;
    bool used;
};

struct Device {
    Info info;
    uint32_t slot;
    uint32_t speed;           // xHCI speed id: 1 full, 2 low, 3 high, 4 super
    void* output;             // device context (owned by the controller)
    void* input;              // input context (ours)
    uint8_t* buffer;          // one page for control transfers
    Endpoint ep[32];          // by DCI; 1 = EP0
    sync::Mutex ep0_lock;
    bool gone;                // being detached: no new transfers

    // where it is
    Device* parent;           // hub, nullptr on a root port
    uint32_t parent_port;     // port on the parent hub (or the root port)
    uint32_t root_port;
    uint32_t route;           // route string (a nibble per hub tier)
    uint32_t depth;           // hubs above it
    uint32_t tt_slot, tt_port;  // the high-speed hub that translates for a low/full-speed device
    char path[24];            // "3", "3.2", "3.2.1"

    // HID
    uint32_t hid_dci;
    uint8_t* report;          // DMA
    uint32_t report_size;
    uint8_t interface;
    uint8_t last[8];          // previous keyboard report
    uint8_t held;             // keyboard: the last key pressed (for repeat)
    uint64_t held_since, repeated_at;

    // hub
    uint32_t hub_ports;
    bool hub_usb3;
    uint32_t hub_dci;
    uint8_t* hub_status;      // DMA: the status-change bitmap
    uint32_t hub_status_size;
    volatile uint32_t pending_ports;  // bit n: port n changed (bit 0: the hub itself)
    uint32_t power_on_ms;
    Device* children[kMaxHubPorts + 1];

    // storage
    uint32_t bulk_in, bulk_out;  // DCIs
    uint8_t bulk_in_address, bulk_out_address;
    uint8_t* dma;             // kStorageBuffer bytes, contiguous
    uint8_t* bot;             // CBW at 0, CSW at 64 (one page)
    uint32_t tag;
    uint32_t block_size;
    uint64_t blocks;
    int32_t disk;             // block device index, -1 = none
    sync::Mutex io;
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
    Device* root[kMaxPorts + 1];  // the device on each root port
    bool interrupts;

    sync::Mutex command_lock;  // one command in flight
    sync::Semaphore command_done;
    volatile uint32_t command_code, command_slot;
    sync::SpinLock lock;       // rings and the slot table against the interrupt handler

    volatile uint64_t pending_root;  // bit n: root port n changed
    sync::Semaphore work;            // wakes the hub thread
    sync::Semaphore scanned;         // the first full scan is over
};

Controller* g_hc = nullptr;
sync::SpinLock g_list_lock;
Device* g_devices[kMaxSlots];
uint32_t g_device_count = 0;
ScancodeSink g_sink = nullptr;
Mouse g_mouse;
uint64_t g_attached = 0, g_detached = 0;

uint32_t r32(volatile uint8_t* b, uint32_t o) { return *reinterpret_cast<volatile uint32_t*>(b + o); }
void w32(volatile uint8_t* b, uint32_t o, uint32_t v) { *reinterpret_cast<volatile uint32_t*>(b + o) = v; }
void w64(volatile uint8_t* b, uint32_t o, uint64_t v) {
    w32(b, o, uint32_t(v));
    w32(b, o + 4, uint32_t(v >> 32));
}

uint32_t* context(Controller* c, void* base, uint32_t index) {
    return reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(base) + index * c->context_size);
}

void list_add(Device* d) {
    sync::ScopedLock guard(g_list_lock);
    if (g_device_count < kMaxSlots) g_devices[g_device_count++] = d;
}

void list_remove(Device* d) {
    sync::ScopedLock guard(g_list_lock);
    for (uint32_t i = 0; i < g_device_count; ++i) {
        if (g_devices[i] != d) continue;
        for (uint32_t j = i + 1; j < g_device_count; ++j) g_devices[j - 1] = g_devices[j];
        --g_device_count;
        return;
    }
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

// Interrupt-IN transfers that stay queued (HID reports, hub status changes). c->lock held.
void queue_interrupt(Controller* c, Device* d, uint32_t dci, uint8_t* buffer, uint32_t size) {
    if (d->gone) return;
    push(d->ep[dci].ring, pmm::virt_to_phys(buffer), size, (kTrbNormal << 10) | kIoc | kIsp);
    c->doorbells[d->slot] = dci;
}

// ---- events (interrupt handler, or polling)

void handle_events(Controller* c) {
    sync::ScopedLock guard(c->lock);
    bool any = false, wake_hub = false;
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
            if (!d) {
            } else if (dci == d->hid_dci && d->hid_dci) {
                if (code == kSuccess || code == kShortPacket) {
                    uint32_t got = d->report_size - (e.status & 0xFFFFFF);
                    ++d->info.reports;
                    if (d->info.kind == kKindKeyboard && got >= 8) keyboard_report(d, d->report);
                    else if (d->info.kind == kKindMouse && got >= 3) mouse_report(d->report, got);
                }
                if (code != kStall) queue_interrupt(c, d, d->hid_dci, d->report, d->report_size);
            } else if (dci == d->hub_dci && d->hub_dci) {
                if (code == kSuccess || code == kShortPacket) {
                    uint32_t got = d->hub_status_size - (e.status & 0xFFFFFF);
                    uint32_t bits = 0;
                    for (uint32_t i = 0; i < got && i < 4; ++i) bits |= uint32_t(d->hub_status[i]) << (8 * i);
                    __atomic_or_fetch(&d->pending_ports, bits, __ATOMIC_ACQ_REL);
                    wake_hub = true;
                }
                if (code != kStall) queue_interrupt(c, d, d->hub_dci, d->hub_status, d->hub_status_size);
            } else if (dci < 32 && d->ep[dci].used) {  // a transfer somebody waits for
                d->ep[dci].code = code;
                d->ep[dci].residue = e.status & 0xFFFFFF;
                d->ep[dci].done.signal();
            }
        } else if (type == kTrbPortStatusChange) {
            uint32_t port = uint32_t(e.parameter >> 24) & 0xFF;
            if (port >= 1 && port <= c->ports && port <= kMaxPorts) {
                __atomic_or_fetch(&c->pending_root, 1ull << port, __ATOMIC_ACQ_REL);
                wake_hub = true;
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
    if (wake_hub) c->work.signal();
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

// A command; its completion code (and the slot id for Enable Slot), 0 when the controller did not answer.
uint32_t command(Controller* c, uint64_t parameter, uint32_t control, uint32_t* slot = nullptr) {
    sync::MutexGuard one(c->command_lock);
    while (c->command_done.try_wait()) {
    }
    {
        sync::ScopedLock guard(c->lock);
        push(c->commands, parameter, 0, control);
        c->doorbells[0] = 0;
    }
    if (c->command_done.wait_for(2000, false) <= 0) return 0;
    if (slot) *slot = c->command_slot;
    return c->command_code;
}

// Waits for the transfer on endpoint `dci`; its completion code (0 = no answer in time).
uint32_t await(Device* d, uint32_t dci, uint64_t timeout_ms) {
    if (d->ep[dci].done.wait_for(timeout_ms, false) <= 0) return 0;
    return d->ep[dci].code;
}

// After a stall the controller halts the endpoint: reset it and point its ring past the failed transfer.
void clear_halt(Controller* c, Device* d, uint32_t dci) {
    command(c, 0, (kTrbResetEndpoint << 10) | (d->slot << 24) | (dci << 16));
    Ring& r = d->ep[dci].ring;
    uint64_t dequeue;
    {
        sync::ScopedLock guard(c->lock);
        dequeue = (r.phys + r.index * sizeof(Trb)) | r.cycle;
    }
    command(c, dequeue, (kTrbSetDequeue << 10) | (d->slot << 24) | (dci << 16));
}

// Control transfer on EP0; `length` bytes in or out of d->buffer. 0 or an Error.
int64_t control(Controller* c, Device* d, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
                uint16_t length) {
    if (d->gone) return kErrIo;
    sync::MutexGuard one(d->ep0_lock);
    Endpoint& ep = d->ep[1];
    while (ep.done.try_wait()) {
    }
    bool in = request_type & 0x80;
    uint64_t setup = uint64_t(request_type) | (uint64_t(request) << 8) | (uint64_t(value) << 16) |
                     (uint64_t(index) << 32) | (uint64_t(length) << 48);
    uint32_t trt = length == 0 ? 0 : in ? 3 : 2;
    {
        sync::ScopedLock guard(c->lock);
        push(ep.ring, setup, 8, (kTrbSetup << 10) | kIdt | (trt << 16));
        if (length) push(ep.ring, pmm::virt_to_phys(d->buffer), length, (kTrbData << 10) | (in ? 1u << 16 : 0));
        push(ep.ring, 0, 0, (kTrbStatus << 10) | kIoc | ((length == 0 || !in) ? 1u << 16 : 0));
        c->doorbells[d->slot] = 1;
    }
    uint32_t code = await(d, 1, 1000);
    if (code == kStall) clear_halt(c, d, 1);  // a request the device does not support: EP0 recovers at once
    return code == kSuccess || code == kShortPacket ? 0 : kErrIo;
}

// One bulk transfer of `length` bytes (≤ 64 KiB, `buffer` must not cross a 64 KiB boundary). Bytes moved, or an
// Error; *stalled tells a stall apart from other failures.
int64_t bulk(Controller* c, Device* d, uint32_t dci, void* buffer, uint32_t length, bool* stalled, uint64_t timeout_ms = 5000) {
    *stalled = false;
    if (d->gone) return kErrIo;
    Endpoint& ep = d->ep[dci];
    while (ep.done.try_wait()) {
    }
    {
        sync::ScopedLock guard(c->lock);
        push(ep.ring, pmm::virt_to_phys(buffer), length, (kTrbNormal << 10) | kIoc | kIsp);
        c->doorbells[d->slot] = dci;
    }
    uint32_t code = await(d, dci, timeout_ms);
    if (code == kSuccess || code == kShortPacket) return int64_t(length - (ep.residue < length ? ep.residue : length));
    if (code == kStall) *stalled = true;
    return kErrIo;
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

// Adds endpoint `dci` (type: 2 bulk out, 6 bulk in, 7 interrupt in) to the input context of a Configure Endpoint.
bool add_endpoint(Controller* c, Device* d, uint32_t dci, uint32_t type, uint32_t packet, uint32_t interval_field,
                  uint32_t burst = 0) {
    if (!d->ep[dci].ring.trbs && !make_ring(d->ep[dci].ring)) return false;
    d->ep[dci].used = true;
    uint32_t* icc = context(c, d->input, 0);
    icc[1] |= 1u << dci;
    uint32_t* ep = context(c, d->input, dci + 1);
    ep[0] = interval_field << 16;
    ep[1] = (3u << 1) | (type << 3) | (burst << 8) | (packet << 16);  // CErr 3
    ep[2] = uint32_t(d->ep[dci].ring.phys) | 1;
    ep[3] = uint32_t(d->ep[dci].ring.phys >> 32);
    ep[4] = (type == 7 ? packet : 1024) | ((type == 7 ? packet : 0) << 16);  // average TRB length, max ESIT payload
    return true;
}

// Starts an input context for Configure Endpoint: slot context as the controller has it, Context Entries raised.
uint32_t* begin_configure(Controller* c, Device* d, uint32_t last_dci) {
    mem::set(d->input, 0, 4096);
    uint32_t* icc = context(c, d->input, 0);
    icc[1] = 1;  // slot
    uint32_t* sc = context(c, d->input, 1);
    mem::copy(sc, context(c, d->output, 0), c->context_size);
    uint32_t entries = (sc[0] >> 27) & 0x1F;
    if (last_dci > entries) entries = last_dci;
    sc[0] = (sc[0] & ~(0x1Fu << 27)) | (entries << 27);
    sc[3] = 0;  // the slot state is the controller's
    return sc;
}

void log_device(Device* d, const char* what);

// ---------------------------------------------------------------- HID

void setup_hid(Controller* c, Device* d, uint8_t kind, uint8_t iface, uint8_t address, uint16_t packet, uint8_t bint) {
    control(c, d, 0x21, 0x0B, 0, iface, 0);       // SET_PROTOCOL(boot)
    control(c, d, 0x21, 0x0A, 8 << 8, iface, 0);  // SET_IDLE(32 ms)
    d->interface = iface;
    uint32_t dci = (address & 0xF) * 2 + 1;
    d->report_size = packet < 8 ? 8 : (packet > 64 ? 64 : packet);
    d->report = static_cast<uint8_t*>(pmm::alloc_page());
    if (!d->report) return;
    begin_configure(c, d, dci);
    if (!add_endpoint(c, d, dci, 7, packet, interval(d->speed, bint))) return;
    if (command(c, pmm::virt_to_phys(d->input), (kTrbConfigureEndpoint << 10) | (d->slot << 24)) != kSuccess) {
        log::warn("usb: Configure Endpoint failed (HID)");
        return;
    }
    d->info.kind = kind;
    d->hid_dci = dci;
    sync::ScopedLock guard(c->lock);
    queue_interrupt(c, d, dci, d->report, d->report_size);
}

// ---------------------------------------------------------------- hubs

constexpr uint16_t kPortConnection = 1, kPortEnable = 2, kPortReset = 0x10, kPortLowSpeed = 0x200, kPortHighSpeed = 0x400;
constexpr uint16_t kFeaturePortReset = 4, kFeaturePortPower = 8, kFeatureCConnection = 16;

bool hub_port_status(Controller* c, Device* hub, uint32_t port, uint16_t& status, uint16_t& change) {
    if (control(c, hub, 0xA3, 0, 0, uint16_t(port), 4) < 0) return false;
    status = uint16_t(hub->buffer[0] | (hub->buffer[1] << 8));
    change = uint16_t(hub->buffer[2] | (hub->buffer[3] << 8));
    return true;
}

void setup_hub(Controller* c, Device* d, uint8_t address, uint16_t packet, uint8_t bint) {
    d->hub_usb3 = d->speed >= 4;
    if (d->hub_usb3) control(c, d, 0x20, 12, uint16_t(d->depth), 0, 0);  // SET_HUB_DEPTH
    // Hub descriptor: ports, power-on time, TT think time.
    uint16_t type = d->hub_usb3 ? 0x2A00 : 0x2900;
    if (control(c, d, 0xA0, 6, type, 0, 12) < 0) {
        log::warn("usb: hub without a hub descriptor");
        return;
    }
    uint32_t ports = d->buffer[2];
    if (ports > kMaxHubPorts) ports = kMaxHubPorts;
    uint16_t characteristics = uint16_t(d->buffer[3] | (d->buffer[4] << 8));
    d->hub_ports = ports;
    d->power_on_ms = uint32_t(d->buffer[5]) * 2;
    d->info.hub_ports = uint8_t(ports);

    uint32_t dci = (address & 0xF) * 2 + 1;
    d->hub_status_size = packet < 4 ? packet : 4;
    if (d->hub_status_size == 0) d->hub_status_size = 1;
    d->hub_status = static_cast<uint8_t*>(pmm::alloc_page());
    if (!d->hub_status) return;
    uint32_t* sc = begin_configure(c, d, dci);
    sc[0] |= 1u << 26;                                     // Hub
    sc[1] = (sc[1] & 0x00FFFFFF) | (ports << 24);          // Number of Ports
    if (d->speed == 3) sc[2] = (sc[2] & ~(3u << 16)) | (((characteristics >> 5) & 3u) << 16);  // TT think time
    if (!add_endpoint(c, d, dci, 7, packet, interval(d->speed, bint))) return;
    if (command(c, pmm::virt_to_phys(d->input), (kTrbConfigureEndpoint << 10) | (d->slot << 24)) != kSuccess) {
        log::warn("usb: Configure Endpoint failed (hub)");
        return;
    }
    d->info.kind = kKindHub;
    for (uint32_t p = 1; p <= ports; ++p) control(c, d, 0x23, 3, kFeaturePortPower, uint16_t(p), 0);
    sched::sleep_ms(d->power_on_ms > 20 ? d->power_on_ms : 20);
    d->hub_dci = dci;
    __atomic_or_fetch(&d->pending_ports, ((1u << (ports + 1)) - 1) & ~1u, __ATOMIC_ACQ_REL);  // look at every port once
    {
        sync::ScopedLock guard(c->lock);
        queue_interrupt(c, d, dci, d->hub_status, d->hub_status_size);
    }
    c->work.signal();
}

// ---------------------------------------------------------------- mass storage (Bulk-Only Transport, SCSI)

constexpr uint32_t kCbwSignature = 0x43425355, kCswSignature = 0x53425355;

// BOT reset recovery: class reset, then both bulk endpoints cleared (device side and controller side).
void reset_recovery(Controller* c, Device* d) {
    control(c, d, 0x21, 0xFF, 0, d->interface, 0);
    control(c, d, 0x02, 1, 0, d->bulk_in_address, 0);   // CLEAR_FEATURE(ENDPOINT_HALT)
    control(c, d, 0x02, 1, 0, d->bulk_out_address, 0);
    clear_halt(c, d, d->bulk_in);
    clear_halt(c, d, d->bulk_out);
}

void unstall(Controller* c, Device* d, uint32_t dci, uint8_t address) {
    clear_halt(c, d, dci);
    control(c, d, 0x02, 1, 0, address, 0);
}

// One SCSI command: CBW, then `length` bytes from/to d->dma, then CSW. 0, or kErrIo (device said it failed /
// the transport broke — the latter after a reset recovery).
int64_t scsi(Controller* c, Device* d, const uint8_t* cb, uint8_t cb_length, uint32_t length, bool in) {
    uint8_t* cbw = d->bot;
    mem::set(cbw, 0, 31);
    uint32_t tag = ++d->tag;
    auto put32 = [](uint8_t* p, uint32_t v) {
        p[0] = uint8_t(v);
        p[1] = uint8_t(v >> 8);
        p[2] = uint8_t(v >> 16);
        p[3] = uint8_t(v >> 24);
    };
    put32(cbw, kCbwSignature);
    put32(cbw + 4, tag);
    put32(cbw + 8, length);
    cbw[12] = in ? 0x80 : 0;
    cbw[13] = 0;  // LUN 0
    cbw[14] = cb_length;
    mem::copy(cbw + 15, cb, cb_length);
    bool stalled;
    if (bulk(c, d, d->bulk_out, cbw, 31, &stalled) != 31) {
        reset_recovery(c, d);
        return kErrIo;
    }
    if (length) {
        int64_t got = bulk(c, d, in ? d->bulk_in : d->bulk_out, d->dma, length, &stalled);
        if (got < 0 && stalled) unstall(c, d, in ? d->bulk_in : d->bulk_out, in ? d->bulk_in_address : d->bulk_out_address);
        else if (got < 0) {
            reset_recovery(c, d);
            return kErrIo;
        }
    }
    uint8_t* csw = d->bot + 64;
    int64_t got = bulk(c, d, d->bulk_in, csw, 13, &stalled);
    if (got < 0 && stalled) {  // the status may come after clearing the stall
        unstall(c, d, d->bulk_in, d->bulk_in_address);
        got = bulk(c, d, d->bulk_in, csw, 13, &stalled);
    }
    auto get32 = [](const uint8_t* p) { return uint32_t(p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24)); };
    if (got != 13 || get32(csw) != kCswSignature || get32(csw + 4) != tag || csw[12] == 2) {
        reset_recovery(c, d);
        return kErrIo;
    }
    return csw[12] == 0 ? 0 : kErrIo;
}

void cdb10(uint8_t* cb, uint8_t op, uint32_t lba, uint16_t count) {
    mem::set(cb, 0, 10);
    cb[0] = op;
    cb[2] = uint8_t(lba >> 24);
    cb[3] = uint8_t(lba >> 16);
    cb[4] = uint8_t(lba >> 8);
    cb[5] = uint8_t(lba);
    cb[7] = uint8_t(count >> 8);
    cb[8] = uint8_t(count);
}

int64_t storage_io(void* context, uint64_t lba, uint32_t count, void* buffer, bool writing) {
    auto* d = static_cast<Device*>(context);
    Controller* c = g_hc;
    sync::MutexGuard one(d->io);
    auto* bytes = static_cast<uint8_t*>(buffer);
    uint32_t per = kStorageBuffer / d->block_size;
    while (count) {
        if (d->gone) return kErrIo;
        uint32_t n = count < per ? count : per;
        uint32_t length = n * d->block_size;
        uint8_t cb[10];
        cdb10(cb, writing ? 0x2A : 0x28, uint32_t(lba), uint16_t(n));
        if (writing) mem::copy(d->dma, bytes, length);
        int64_t e = scsi(c, d, cb, 10, length, !writing);
        if (e < 0) e = scsi(c, d, cb, 10, length, !writing);  // once more after a recovery
        if (e < 0) return e;
        if (!writing) mem::copy(bytes, d->dma, length);
        bytes += length;
        lba += n;
        count -= n;
    }
    return 0;
}

int64_t storage_read(void* context, uint64_t lba, uint32_t count, void* buffer) {
    return storage_io(context, lba, count, buffer, false);
}

int64_t storage_write(void* context, uint64_t lba, uint32_t count, const void* buffer) {
    return storage_io(context, lba, count, const_cast<void*>(buffer), true);
}

int64_t storage_flush(void* context) {
    auto* d = static_cast<Device*>(context);
    sync::MutexGuard one(d->io);
    uint8_t cb[10];
    cdb10(cb, 0x35, 0, 0);
    return d->gone ? kErrIo : scsi(g_hc, d, cb, 10, 0, false);
}

const block::Ops kStorageOps = {storage_read, storage_write, storage_flush};

void setup_storage(Controller* c, Device* d, uint8_t iface, uint8_t in_address, uint16_t in_packet, uint8_t out_address,
                   uint16_t out_packet, uint8_t burst) {
    d->interface = iface;
    d->bulk_in_address = in_address;
    d->bulk_out_address = out_address;
    d->bulk_in = (in_address & 0xF) * 2 + 1;
    d->bulk_out = (out_address & 0xF) * 2;
    d->dma = static_cast<uint8_t*>(pmm::alloc_contiguous(kStorageBuffer / 4096));
    d->bot = static_cast<uint8_t*>(pmm::alloc_page());
    d->disk = -1;
    if (!d->dma || !d->bot) return;
    uint32_t last = d->bulk_in > d->bulk_out ? d->bulk_in : d->bulk_out;
    begin_configure(c, d, last);
    if (!add_endpoint(c, d, d->bulk_out, 2, out_packet, 0, burst) || !add_endpoint(c, d, d->bulk_in, 6, in_packet, 0, burst))
        return;
    if (command(c, pmm::virt_to_phys(d->input), (kTrbConfigureEndpoint << 10) | (d->slot << 24)) != kSuccess) {
        log::warn("usb: Configure Endpoint failed (storage)");
        return;
    }
    d->info.kind = kKindStorage;

    d->io.lock();
    uint8_t cb[12];
    mem::set(cb, 0, sizeof(cb));
    cb[0] = 0x12;  // INQUIRY
    cb[4] = 36;
    char model[40] = {};
    if (scsi(c, d, cb, 6, 36, true) == 0) {
        uint32_t n = 0;
        for (uint32_t i = 8; i < 36 && n < sizeof(model) - 1; ++i) {
            char ch = char(d->dma[i]);
            if (ch < ' ' || ch > '~') ch = ' ';
            if (ch == ' ' && n && model[n - 1] == ' ') continue;
            model[n++] = ch;
        }
        while (n && model[n - 1] == ' ') --n;
        model[n] = 0;
    }
    bool ready = false;
    for (int tries = 0; tries < 20 && !ready && !d->gone; ++tries) {  // TEST UNIT READY: media may spin up
        mem::set(cb, 0, sizeof(cb));
        ready = scsi(c, d, cb, 6, 0, false) == 0;
        if (!ready) {
            cb[0] = 0x03;  // REQUEST SENSE clears the "unit attention" after a reset
            cb[4] = 18;
            scsi(c, d, cb, 6, 18, true);
            sched::sleep_ms(50);
        }
    }
    mem::set(cb, 0, sizeof(cb));
    cb[0] = 0x25;  // READ CAPACITY(10)
    if (!ready || scsi(c, d, cb, 10, 8, true) < 0) {
        d->io.unlock();
        log::warn("usb: storage device does not report its capacity");
        return;
    }
    auto be32 = [](const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; };
    d->blocks = uint64_t(be32(d->dma)) + 1;
    d->block_size = be32(d->dma + 4);
    d->io.unlock();
    if (d->block_size < 512 || d->block_size > 4096 || (d->block_size & (d->block_size - 1))) {
        log::warn("usb: storage device with an unusual block size");
        return;
    }
    char name[8] = "usb0";
    for (char i = 0; i < 10; ++i) {
        name[3] = char('0' + i);
        if (block::find(name) < 0) break;
    }
    str::copy(reinterpret_cast<char*>(d->info.disk), name, sizeof(d->info.disk));
    // without d->io held: block::add calls the hot-plug handler, which mounts (reads) the disk
    d->disk = block::add(name, model[0] ? model : "USB storage", d->blocks, d->block_size, false, &kStorageOps, d);
}

// ---------------------------------------------------------------- attach / detach

void free_device(Device* d) {
    for (uint32_t i = 0; i < 32; ++i)
        if (d->ep[i].ring.trbs) pmm::free_page(d->ep[i].ring.trbs);
    if (d->output) pmm::free_page(d->output);
    if (d->input) pmm::free_page(d->input);
    if (d->buffer) pmm::free_page(d->buffer);
    if (d->report) pmm::free_page(d->report);
    if (d->hub_status) pmm::free_page(d->hub_status);
    if (d->bot) pmm::free_page(d->bot);
    if (d->dma) pmm::free_contiguous(d->dma, kStorageBuffer / 4096);
    heap::free(d);
}

// Unplugged (or its hub was): children first, then the class driver lets go, then the slot.
void detach(Controller* c, Device* d) {
    for (uint32_t p = 1; p <= kMaxHubPorts; ++p) {
        if (d->children[p]) detach(c, d->children[p]);
        d->children[p] = nullptr;
    }
    d->gone = true;
    // Wake anyone waiting for a transfer on it; they see `gone` (or their transfer times out).
    for (uint32_t i = 1; i < 32; ++i)
        if (d->ep[i].used) {
            d->ep[i].code = 0;
            d->ep[i].done.signal();
        }
    if (d->info.kind == kKindStorage) {
        d->io.lock();  // the request in flight is over
        if (d->disk >= 0) block::remove(uint32_t(d->disk));
        d->io.unlock();
    }
    list_remove(d);
    log_device(d, "removed");
    command(c, 0, (kTrbDisableSlot << 10) | (d->slot << 24));
    {
        sync::ScopedLock guard(c->lock);
        c->slots[d->slot] = nullptr;
        c->dcbaa[d->slot] = 0;
    }
    if (d->parent) d->parent->children[d->parent_port] = nullptr;
    else c->root[d->root_port] = nullptr;
    ++g_detached;
    free_device(d);
}

void log_device(Device* d, const char* what) {
    static const char* const kKinds[] = {"device", "keyboard", "mouse", "hub", "storage"};
    char line[160];
    uint32_t n = 0;
    auto add = [&](const char* s) {
        while (*s && n < sizeof(line) - 1) line[n++] = *s++;
    };
    add("usb ");
    add(d->path);
    add(": ");
    add(kKinds[d->info.kind < 5 ? d->info.kind : 0]);
    if (d->info.product[0]) {
        add(" \"");
        add(reinterpret_cast<const char*>(d->info.product));
        add("\"");
    }
    if (d->info.disk[0]) {
        add(" → ");
        add(reinterpret_cast<const char*>(d->info.disk));
    }
    add(" ");
    add(what);
    line[n] = 0;
    log::info(line);
}

// A new device on `port` of `parent` (nullptr: root port `port`), already reset and enabled.
Device* attach(Controller* c, Device* parent, uint32_t port, uint32_t speed) {
    uint32_t slot = 0;
    if (command(c, 0, kTrbEnableSlot << 10, &slot) != kSuccess || slot == 0 || slot > kMaxSlots) {
        log::warn("usb: no device slot");
        return nullptr;
    }
    auto* d = static_cast<Device*>(heap::alloc_zeroed(sizeof(Device)));
    if (!d) return nullptr;
    d->slot = slot;
    d->speed = speed;
    d->parent = parent;
    d->parent_port = port;
    d->disk = -1;
    d->info.slot = uint8_t(slot);
    d->info.speed = uint8_t(speed);
    if (parent) {
        d->root_port = parent->root_port;
        d->depth = parent->depth + 1;
        d->route = parent->route | ((port > 15 ? 15 : port) << (4 * parent->depth));
        if (parent->speed == 3 && speed < 3) {  // a high-speed hub translates for low/full speed
            d->tt_slot = parent->slot;
            d->tt_port = port;
        } else {
            d->tt_slot = parent->tt_slot;
            d->tt_port = parent->tt_port;
        }
        d->info.parent = uint8_t(parent->slot);
        d->info.port = uint8_t(port);
        uint32_t n = 0;
        while (parent->path[n] && n < sizeof(d->path) - 4) {
            d->path[n] = parent->path[n];
            ++n;
        }
        d->path[n++] = '.';
        if (port >= 10) d->path[n++] = char('0' + port / 10);
        d->path[n++] = char('0' + port % 10);
        d->path[n] = 0;
    } else {
        d->root_port = port;
        d->info.port = uint8_t(port);
        if (port >= 10) {
            d->path[0] = char('0' + port / 10);
            d->path[1] = char('0' + port % 10);
        } else {
            d->path[0] = char('0' + port);
        }
    }
    d->info.depth = uint8_t(d->depth);
    d->output = pmm::alloc_page();
    d->input = pmm::alloc_page();
    d->buffer = static_cast<uint8_t*>(pmm::alloc_page());
    if (!d->output || !d->input || !d->buffer || !make_ring(d->ep[1].ring)) {
        free_device(d);
        return nullptr;
    }
    d->ep[1].used = true;
    {
        sync::ScopedLock guard(c->lock);
        c->dcbaa[slot] = pmm::virt_to_phys(d->output);
        c->slots[slot] = d;
    }

    // Address Device: slot context (speed, route, root port, TT) + EP0.
    uint32_t* icc = context(c, d->input, 0);
    icc[1] = 1 | 2;  // add slot, EP0
    uint32_t* sc = context(c, d->input, 1);
    sc[0] = d->route | (speed << 20) | (1u << 27);
    sc[1] = d->root_port << 16;
    sc[2] = d->tt_slot | (d->tt_port << 8);
    uint32_t* ep = context(c, d->input, 2);
    ep[1] = (3u << 1) | (4u << 3) | (ep0_packet(speed) << 16);  // CErr 3, control, max packet
    ep[2] = uint32_t(d->ep[1].ring.phys) | 1;
    ep[3] = uint32_t(d->ep[1].ring.phys >> 32);
    ep[4] = 8;
    bool ok = command(c, pmm::virt_to_phys(d->input), (kTrbAddressDevice << 10) | (slot << 24)) == kSuccess;

    // Device descriptor: the first 8 bytes tell EP0's real packet size (full speed may be 8–64).
    ok = ok && control(c, d, 0x80, 6, 0x0100, 0, 8) == 0;
    if (ok) {
        uint32_t packet = speed == 4 ? 1u << d->buffer[7] : d->buffer[7];
        if (packet != ep0_packet(speed) && packet >= 8 && packet <= 512) {
            mem::set(d->input, 0, 4096);
            icc[1] = 2;
            ep[1] = (3u << 1) | (4u << 3) | (packet << 16);
            ep[2] = uint32_t(d->ep[1].ring.phys) | d->ep[1].ring.cycle;
            ep[3] = uint32_t(d->ep[1].ring.phys >> 32);
            ep[4] = 8;
            command(c, pmm::virt_to_phys(d->input), (kTrbEvaluateContext << 10) | (slot << 24));
        }
    }
    ok = ok && control(c, d, 0x80, 6, 0x0100, 0, 18) == 0;
    if (!ok) {
        log::warn("usb: a device does not answer its address / descriptors");
        command(c, 0, (kTrbDisableSlot << 10) | (slot << 24));
        sync::ScopedLock guard(c->lock);
        c->slots[slot] = nullptr;
        c->dcbaa[slot] = 0;
        free_device(d);
        return nullptr;
    }
    d->info.vendor_id = uint16_t(d->buffer[8] | (d->buffer[9] << 8));
    d->info.product_id = uint16_t(d->buffer[10] | (d->buffer[11] << 8));
    d->info.class_code = d->buffer[4];
    d->info.subclass = d->buffer[5];
    d->info.protocol = d->buffer[6];
    uint8_t product_index = d->buffer[15];
    read_string(c, d, product_index, d->info.product, sizeof(d->info.product));
    if (parent) parent->children[port] = d;
    else c->root[port] = d;
    list_add(d);
    ++g_attached;

    // Configuration 1: find what we drive.
    if (control(c, d, 0x80, 6, 0x0200, 0, 9) < 0) return d;
    uint16_t total = uint16_t(d->buffer[2] | (d->buffer[3] << 8));
    if (total > 4096) total = 4096;
    if (control(c, d, 0x80, 6, 0x0200, 0, total) < 0) return d;
    // copy: later control transfers reuse the buffer
    auto* config = static_cast<uint8_t*>(heap::alloc(total));
    if (!config) return d;
    mem::copy(config, d->buffer, total);
    uint8_t configuration = config[5];
    enum { None, Hid, Hub, Storage } what = None;
    uint8_t iface = 0, hid_kind = 0, int_address = 0, int_interval = 0, in_address = 0, out_address = 0, burst = 0;
    uint16_t int_packet = 0, in_packet = 0, out_packet = 0;
    int current = -1;  // which interface the endpoints belong to: 0 skip, 1 HID, 2 hub, 3 storage
    for (uint32_t at = 0; at + 2 <= total && config[at] >= 2; at += config[at]) {
        const uint8_t* desc = config + at;
        if (desc[1] == 4) {  // interface
            current = 0;
            if (what != None || desc[3] != 0) continue;  // the first interface we can drive (alternate 0)
            if (desc[5] == 3 && desc[6] == 1 && (desc[7] == 1 || desc[7] == 2)) {
                what = Hid, current = 1, iface = desc[2], hid_kind = desc[7] == 1 ? kKindKeyboard : kKindMouse;
                d->info.class_code = 3, d->info.subclass = 1, d->info.protocol = desc[7];
            } else if (desc[5] == 9) {
                what = Hub, current = 2, iface = desc[2];
            } else if (desc[5] == 8 && desc[6] == 6 && desc[7] == 0x50) {
                what = Storage, current = 3, iface = desc[2];
                d->info.class_code = 8, d->info.subclass = 6, d->info.protocol = 0x50;
            }
        } else if (desc[1] == 5 && current > 0) {  // endpoint
            uint8_t address = desc[2], type = desc[3] & 3;
            uint16_t packet = uint16_t((desc[4] | (desc[5] << 8)) & 0x7FF);
            if (type == 3 && (address & 0x80) && !int_address) {
                int_address = address, int_packet = packet, int_interval = desc[6];
            } else if (type == 2 && (address & 0x80) && !in_address) {
                in_address = address, in_packet = packet;
            } else if (type == 2 && !(address & 0x80) && !out_address) {
                out_address = address, out_packet = packet;
            }
        } else if (desc[1] == 0x30 && current == 3) {  // SuperSpeed endpoint companion: burst
            burst = desc[2];
        }
    }
    heap::free(config);
    if (control(c, d, 0x00, 9, configuration, 0, 0) < 0) return d;  // SET_CONFIGURATION
    if (what == Hid && int_address) setup_hid(c, d, hid_kind, iface, int_address, int_packet, int_interval);
    else if (what == Hub && int_address) setup_hub(c, d, int_address, int_packet, int_interval);
    else if (what == Storage && in_address && out_address) setup_storage(c, d, iface, in_address, in_packet, out_address, out_packet, burst);
    log_device(d, "attached");
    return d;
}

// ---------------------------------------------------------------- the hub thread

void root_port_changed(Controller* c, uint32_t port) {
    uint32_t off = kPortBase + 0x10 * (port - 1);
    uint32_t v = r32(c->op, off);
    w32(c->op, off, (v & ~(kPortPed | kPortChangeBits)) | (v & kPortChangeBits));  // acknowledge the changes
    bool connected = v & kPortCcs;
    Device* d = c->root[port];
    if (d && (!connected || (v & kPortCsc))) detach(c, d);  // gone, or replaced by another device
    if (!connected || c->root[port]) return;
    if (!(v & kPortPed)) {  // USB 2 ports need a reset to be enabled; USB 3 ones train by themselves
        w32(c->op, off, (v & ~(kPortPed | kPortChangeBits)) | kPortPr);
        uint64_t until = time::uptime_ms() + 500;
        while (!(r32(c->op, off) & kPortPrc) && time::uptime_ms() < until) sched::sleep_ms(1);
        v = r32(c->op, off);
        w32(c->op, off, (v & ~(kPortPed | kPortChangeBits)) | (v & kPortChangeBits));
        sched::sleep_ms(10);
        v = r32(c->op, off);
    }
    if (!(v & kPortPed)) return;
    attach(c, nullptr, port, (v >> 10) & 0xF);
}

void hub_port_changed(Controller* c, Device* hub, uint32_t port) {
    uint16_t status, change;
    if (!hub_port_status(c, hub, port, status, change)) return;
    // Acknowledge every change we were told about (USB 2: 16–20, USB 3: + 25, 26, 29).
    static const uint16_t kChanges[] = {16, 17, 18, 19, 20, 25, 26, 29};
    for (uint16_t f : kChanges) {
        uint16_t bit = uint16_t(1u << (f >= 25 ? (f == 25 ? 6 : f == 26 ? 7 : 5) : f - 16));
        if (change & bit) control(c, hub, 0x23, 1, f, uint16_t(port), 0);
    }
    bool connected = status & kPortConnection;
    Device* d = hub->children[port];
    if (d && (!connected || (change & 1))) detach(c, d);
    if (!connected || hub->children[port]) return;
    sched::sleep_ms(20);  // debounce
    control(c, hub, 0x23, 3, kFeaturePortReset, uint16_t(port), 0);
    uint64_t until = time::uptime_ms() + 500;
    do {
        sched::sleep_ms(10);
        if (!hub_port_status(c, hub, port, status, change)) return;
    } while ((status & kPortReset) && time::uptime_ms() < until);
    control(c, hub, 0x23, 1, 20, uint16_t(port), 0);  // C_PORT_RESET
    if (hub->hub_usb3) control(c, hub, 0x23, 1, 29, uint16_t(port), 0);
    sched::sleep_ms(10);
    if (!(status & kPortEnable) || !(status & kPortConnection)) return;
    uint32_t speed = hub->hub_usb3 ? 4 : (status & kPortLowSpeed) ? 2 : (status & kPortHighSpeed) ? 3 : 1;
    attach(c, hub, port, speed);
}

// Every device that is a hub (a snapshot: the list changes while we work).
uint32_t hubs(Device** out, uint32_t capacity) {
    sync::ScopedLock guard(g_list_lock);
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_device_count && n < capacity; ++i)
        if (g_devices[i]->info.kind == kKindHub && g_devices[i]->hub_dci) out[n++] = g_devices[i];
    return n;
}

void hub_thread(uint64_t context) {
    auto* c = reinterpret_cast<Controller*>(context);
    bool announced = false;
    for (;;) {
        bool any = false;
        uint64_t roots = __atomic_exchange_n(&c->pending_root, 0, __ATOMIC_ACQ_REL);
        for (uint32_t p = 1; p <= c->ports && p <= kMaxPorts; ++p) {
            if (!(roots & (1ull << p))) continue;
            root_port_changed(c, p);
            any = true;
        }
        Device* list[kMaxSlots];
        uint32_t n = hubs(list, kMaxSlots);
        for (uint32_t i = 0; i < n; ++i) {
            Device* hub = list[i];
            uint32_t bits;
            {
                sync::ScopedLock guard(g_list_lock);  // the hub may have been detached meanwhile
                bool alive = false;
                for (uint32_t j = 0; j < g_device_count; ++j) alive |= g_devices[j] == hub;
                if (!alive) continue;
                bits = __atomic_exchange_n(&hub->pending_ports, 0, __ATOMIC_ACQ_REL);
            }
            for (uint32_t p = 1; p <= hub->hub_ports; ++p) {
                if (!(bits & (1u << p))) continue;
                hub_port_changed(c, hub, p);
                any = true;
            }
        }
        if (any) continue;  // new hubs may have ports to look at: go round again before sleeping
        if (!announced) {
            announced = true;
            c->scanned.signal();
        }
        c->work.wait();
    }
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
            w32(x, 4, r32(x, 4) & ~0x1F);  // no more SMIs for USB events
        }
        uint32_t next = (v >> 8) & 0xFF;
        offset = next ? offset + (next << 2) : 0;
    }
}

}  // namespace

uint32_t count() {
    sync::ScopedLock guard(g_list_lock);
    return g_device_count;
}

bool info(uint32_t index, Info& out) {
    sync::ScopedLock guard(g_list_lock);
    if (index >= g_device_count) return false;
    out = g_devices[index]->info;
    return true;
}

void set_keyboard_sink(ScancodeSink sink) { __atomic_store_n(&g_sink, sink, __ATOMIC_RELEASE); }

Mouse mouse() { return g_mouse; }

Stats stats() { return {g_attached, g_detached}; }

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
    if (!c->interrupts) sched::spawn("xhci poll", &poll_thread, reinterpret_cast<uint64_t>(c), sched::Priority::High);

    // The hub thread scans every root port (and every hub it finds); wait for that first scan.
    for (uint32_t p = 1; p <= c->ports && p <= kMaxPorts; ++p)
        if (r32(c->op, kPortBase + 0x10 * (p - 1)) & kPortCcs) c->pending_root |= 1ull << p;
    sched::spawn("usb-hub", &hub_thread, reinterpret_cast<uint64_t>(c), sched::Priority::High);
    c->work.signal();
    c->scanned.wait_for(5000, false);

    term::write("[neko] xhci: ");
    term::write_udec(c->ports);
    term::write(" ports, ");
    term::write_udec(neko::usb::count());
    term::write(" device(s)");
    Info d;
    for (uint32_t i = 0; info(i, d); ++i) {
        static const char* const kKinds[] = {"device", "keyboard", "mouse", "hub", "storage"};
        term::write(i ? ", " : ": ");
        term::write(kKinds[d.kind < 5 ? d.kind : 0]);
        if (d.product[0]) {
            term::write(" \"");
            term::write(d.product);
            term::write("\"");
        }
    }
    term::write(c->interrupts ? ", interrupts, hot plug\n" : ", polling, hot plug\n");
}

}  // namespace neko::drivers
