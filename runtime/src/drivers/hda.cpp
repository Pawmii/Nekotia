// Intel High Definition Audio: the controller (CORB/RIRB command rings, one output stream) and its codecs
// (QEMU: -device ich9-intel-hda / intel-hda with hda-output or hda-duplex; most PCs since 2004).
//
//   reset → codecs from STATESTS → per codec: the audio function group, its widgets, and a path from a
//   connected output pin back to a DAC through mixers/selectors (connection lists). Every node on the path
//   is powered up, selected and unmuted at 0 dB; the DAC gets stream tag 1, 48 kHz / 16-bit / stereo.
//   The first output stream engine then plays a 64 KiB ring (16 buffer descriptors) in a loop forever, and
//   neko::audio mixes into it, reading the position from the DMA position buffer.
//
// Commands are polled (they take microseconds); nothing here needs interrupts.
#include <neko/audio.hpp>
#include <neko/runtime.hpp>

namespace neko::drivers {
namespace {

// Controller registers.
constexpr uint32_t kGcap = 0x00, kGctl = 0x08, kStatests = 0x0E, kIntctl = 0x20, kCorbLo = 0x40, kCorbHi = 0x44,
                   kCorbWp = 0x48, kCorbRp = 0x4A, kCorbCtl = 0x4C, kCorbSize = 0x4E, kRirbLo = 0x50, kRirbHi = 0x54,
                   kRirbWp = 0x58, kRintCnt = 0x5A, kRirbCtl = 0x5C, kRirbSts = 0x5D, kRirbSize = 0x5E, kDplLo = 0x70,
                   kDplHi = 0x74;
// Stream descriptor registers (from its base).
constexpr uint32_t kSdCtl = 0x00, kSdSts = 0x03, kSdLpib = 0x04, kSdCbl = 0x08, kSdLvi = 0x0C, kSdFmt = 0x12,
                   kSdBdlLo = 0x18, kSdBdlHi = 0x1C;

// Codec verbs and parameters.
constexpr uint32_t kGetParameter = 0xF00, kGetConnection = 0xF02, kGetPinConfig = 0xF1C, kSetSelect = 0x701,
                   kSetPower = 0x705, kSetStream = 0x706, kSetPinControl = 0x707, kSetEapd = 0x70C;
constexpr uint32_t kParamNodes = 0x04, kParamGroupType = 0x05, kParamWidgetCaps = 0x09, kParamPinCaps = 0x0C,
                   kParamConnLength = 0x0E, kParamOutAmp = 0x12, kParamInAmp = 0x0D;
constexpr uint16_t kFormat48k16Stereo = 0x0011;

constexpr uint32_t kRingBytes = 64 * 1024, kFragments = 16;

struct Line {
    char text[128];
    uint32_t n = 0;
    Line& operator<<(const char* s) {
        while (*s && n < sizeof(text) - 1) text[n++] = *s++;
        text[n] = 0;
        return *this;
    }
    Line& operator<<(uint64_t v) {
        char digits[24];
        fmt::udec(digits, v);
        return *this << digits;
    }
};

enum WidgetType : uint8_t { kOutput = 0, kInput = 1, kMixer = 2, kSelector = 3, kPin = 4 };

struct Hda {
    volatile uint8_t* regs;
    uint32_t* corb;
    uint64_t* rirb;
    uint16_t corb_entries, rirb_entries, rirb_read;
    uint32_t stream_base;  // offset of the output stream descriptor
    uint8_t* ring;
    uint64_t* bdl;
    volatile uint32_t* positions;  // DMA position buffer
    uint32_t stream_index;
    char name[64];
};

Hda g_hda;

uint8_t r8(uint32_t o) { return g_hda.regs[o]; }
uint16_t r16(uint32_t o) { return *reinterpret_cast<volatile uint16_t*>(g_hda.regs + o); }
uint32_t r32(uint32_t o) { return *reinterpret_cast<volatile uint32_t*>(g_hda.regs + o); }
void w8(uint32_t o, uint8_t v) { g_hda.regs[o] = v; }
void w16(uint32_t o, uint16_t v) { *reinterpret_cast<volatile uint16_t*>(g_hda.regs + o) = v; }
void w32(uint32_t o, uint32_t v) { *reinterpret_cast<volatile uint32_t*>(g_hda.regs + o) = v; }

void fail(const char* why) { log::warn((Line() << "hda: " << why).text); }

// Spins until (reg & mask) == want, at most `ms`.
template <typename Read>
bool wait_for(Read read, uint32_t mask, uint32_t want, uint32_t ms) {
    uint64_t until = time::uptime_ms() + ms;
    while ((read() & mask) != want) {
        if (time::uptime_ms() > until) return false;
        cpu::pause();
    }
    return true;
}

// Sends one verb through the CORB and waits for its answer in the RIRB. ~0u on a timeout.
uint32_t command(uint32_t codec, uint32_t node, uint32_t verb, uint32_t payload) {
    uint32_t word = (codec << 28) | (node << 20);
    word |= verb >= 0x100 ? (verb << 8) | (payload & 0xFF) : (verb << 16) | (payload & 0xFFFF);
    uint16_t wp = uint16_t((r16(kCorbWp) + 1) % g_hda.corb_entries);
    g_hda.corb[wp] = word;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    w16(kCorbWp, wp);
    for (uint64_t i = 0; i < 20000000; ++i) {
        uint16_t rp = r16(kRirbWp) & 0xFF;
        if (rp != g_hda.rirb_read) {
            g_hda.rirb_read = uint16_t((g_hda.rirb_read + 1) % g_hda.rirb_entries);
            w8(kRirbSts, 0x05);
            return uint32_t(g_hda.rirb[g_hda.rirb_read]);
        }
        cpu::pause();
    }
    return ~0u;
}

uint32_t parameter(uint32_t codec, uint32_t node, uint32_t which) { return command(codec, node, kGetParameter, which); }

// Ring sizes: the largest the controller supports (256, 16 or 2 entries).
uint16_t pick_size(uint32_t reg) {
    uint8_t caps = r8(reg) >> 4;
    uint8_t code = caps & 4 ? 2 : caps & 2 ? 1 : 0;
    w8(reg, uint8_t((r8(reg) & ~3) | code));
    return code == 2 ? 256 : code == 1 ? 16 : 2;
}

bool reset_controller() {
    w32(kGctl, r32(kGctl) & ~1u);
    if (!wait_for([] { return r32(kGctl); }, 1, 0, 100)) return false;
    speaker::wait_us(200);
    w32(kGctl, r32(kGctl) | 1);
    if (!wait_for([] { return r32(kGctl); }, 1, 1, 100)) return false;
    speaker::wait_us(1000);  // codecs need 521 µs to announce themselves
    return true;
}

bool start_rings() {
    w8(kCorbCtl, 0);
    w8(kRirbCtl, 0);
    wait_for([] { return uint32_t(r8(kCorbCtl)); }, 2, 0, 50);
    wait_for([] { return uint32_t(r8(kRirbCtl)); }, 2, 0, 50);
    g_hda.corb_entries = pick_size(kCorbSize);
    g_hda.rirb_entries = pick_size(kRirbSize);
    auto* page = static_cast<uint8_t*>(pmm::alloc_page());
    auto* page2 = static_cast<uint8_t*>(pmm::alloc_page());
    if (!page || !page2) return false;
    mem::set(page, 0, 4096);
    mem::set(page2, 0, 4096);
    g_hda.corb = reinterpret_cast<uint32_t*>(page);
    g_hda.rirb = reinterpret_cast<uint64_t*>(page2);
    uint64_t corb = pmm::virt_to_phys(page), rirb = pmm::virt_to_phys(page2);
    w32(kCorbLo, uint32_t(corb));
    w32(kCorbHi, uint32_t(corb >> 32));
    w32(kRirbLo, uint32_t(rirb));
    w32(kRirbHi, uint32_t(rirb >> 32));
    // Read pointer reset: set bit 15, wait for it, clear it (some controllers, QEMU's too, never show it set).
    w16(kCorbRp, 0x8000);
    wait_for([] { return uint32_t(r16(kCorbRp)); }, 0x8000, 0x8000, 5);
    w16(kCorbRp, 0);
    wait_for([] { return uint32_t(r16(kCorbRp)); }, 0x8000, 0, 5);
    w16(kCorbWp, 0);
    w16(kRirbWp, 0x8000);  // write pointer reset
    g_hda.rirb_read = 0;
    w16(kRintCnt, 0xFF);
    w8(kCorbCtl, 2);  // run
    w8(kRirbCtl, 2);  // DMA on, no interrupts
    return true;
}

// ---------------------------------------------------------------- codec: a path from a pin to a DAC

struct Codec {
    uint32_t address;
    uint32_t first, count;  // widget nodes of the audio function group
    uint32_t group;
};

uint8_t type_of(const Codec& c, uint32_t node) { return uint8_t((parameter(c.address, node, kParamWidgetCaps) >> 20) & 0xF); }

// Connection `index` of `node` (short or long form).
uint32_t connection(const Codec& c, uint32_t node, uint32_t index, uint32_t length_word) {
    bool long_form = length_word & 0x80;
    uint32_t per = long_form ? 2 : 4;
    uint32_t r = command(c.address, node, kGetConnection, index - index % per);
    uint32_t shift = (index % per) * (long_form ? 16 : 8);
    return (r >> shift) & (long_form ? 0x7FFF : 0xFF);
}

constexpr uint32_t kMaxPath = 6;

// Depth-first from `node` towards a DAC; path[0] is the DAC when it returns true, choice[i] the input of path[i+1].
bool find_dac(const Codec& c, uint32_t node, uint32_t depth, uint32_t* path, uint32_t* choice, uint32_t& length) {
    if (depth >= kMaxPath) return false;
    uint8_t type = type_of(c, node);
    if (type == kOutput) {
        path[0] = node;
        length = depth + 1;
        return true;
    }
    if (type != kPin && type != kMixer && type != kSelector) return false;
    uint32_t length_word = parameter(c.address, node, kParamConnLength);
    uint32_t n = length_word & 0x7F;
    for (uint32_t i = 0; i < n && i < 16; ++i) {
        uint32_t from = connection(c, node, i, length_word);
        if (from == 0 || from == node) continue;
        uint32_t sub_length = 0;
        // Recurse with a shifted view: the sub-path fills path[0..], this node goes after it.
        if (find_dac(c, from, depth + 1, path, choice, sub_length)) {
            uint32_t at = sub_length - 1 - depth;  // our place in the final path
            path[at] = node;
            choice[at] = i;
            length = sub_length;
            return true;
        }
    }
    return false;
}

void set_amp(const Codec& c, uint32_t node, bool output, uint32_t index) {
    uint32_t caps = parameter(c.address, node, output ? kParamOutAmp : kParamInAmp);
    if (caps == 0) caps = parameter(c.address, c.group, output ? kParamOutAmp : kParamInAmp);  // the group's default
    uint32_t gain = caps & 0x7F;  // offset = the step that is 0 dB
    uint32_t payload = (output ? 0x8000 : 0x4000) | 0x3000 | (index << 8) | gain;  // both channels, unmuted
    command(c.address, node, 0x3, payload);
}

// Configures every node of a path (path[0] = DAC … path[length-1] = pin).
void configure_path(const Codec& c, const uint32_t* path, const uint32_t* choice, uint32_t length) {
    for (uint32_t i = 0; i < length; ++i) {
        uint32_t node = path[i];
        uint32_t caps = parameter(c.address, node, kParamWidgetCaps);
        command(c.address, node, kSetPower, 0);  // D0
        if (i > 0) {
            uint8_t type = uint8_t((caps >> 20) & 0xF);
            if (type != kMixer) command(c.address, node, kSetSelect, choice[i - 1]);
            if (caps & 2) set_amp(c, node, false, type == kMixer ? choice[i - 1] : 0);
        }
        if (caps & 4) set_amp(c, node, true, 0);
    }
    uint32_t pin = path[length - 1];
    uint32_t pin_caps = parameter(c.address, pin, kParamPinCaps);
    uint32_t config = command(c.address, pin, kGetPinConfig, 0);
    uint32_t device = (config >> 20) & 0xF;
    command(c.address, pin, kSetPinControl, device == 2 ? 0xC0 : 0x40);  // out (+ headphone amp)
    if (pin_caps & (1u << 16)) command(c.address, pin, kSetEapd, 2);       // external amplifier on
    command(c.address, path[0], kSetStream, 1u << 4);                     // stream tag 1, channel 0
    command(c.address, path[0], 0x2, kFormat48k16Stereo);
}

// Sets up every output pin of the codec that leads to a DAC. Number of paths.
uint32_t setup_codec(uint32_t address, char* description, uint32_t room) {
    Codec c{address, 0, 0, 0};
    uint32_t nodes = parameter(address, 0, kParamNodes);
    if (nodes == ~0u) return 0;
    uint32_t first_group = (nodes >> 16) & 0xFF, groups = nodes & 0xFF;
    for (uint32_t g = first_group; g < first_group + groups; ++g) {
        if ((parameter(address, g, kParamGroupType) & 0xFF) != 1) continue;  // audio function group
        c.group = g;
        uint32_t w = parameter(address, g, kParamNodes);
        c.first = (w >> 16) & 0xFF;
        c.count = w & 0xFF;
        break;
    }
    if (!c.group) return 0;
    command(address, c.group, kSetPower, 0);
    speaker::wait_us(10000);

    uint32_t paths = 0;
    for (int pass = 0; pass < 2 && paths == 0; ++pass) {  // connected pins first; then any output pin
        for (uint32_t node = c.first; node < c.first + c.count; ++node) {
            if (type_of(c, node) != kPin) continue;
            if (!(parameter(address, node, kParamPinCaps) & 0x10)) continue;  // not output capable
            uint32_t config = command(address, node, kGetPinConfig, 0);
            if (pass == 0 && (config >> 30) == 1) continue;  // "no physical connection"
            uint32_t path[kMaxPath], choice[kMaxPath], length = 0;
            if (!find_dac(c, node, 0, path, choice, length)) continue;
            configure_path(c, path, choice, length);
            if (paths == 0) {
                static const char* const kDevices[16] = {"line out", "speaker", "headphones", "CD", "S/PDIF", "digital out",
                                                         "modem", "modem", "line in", "aux", "mic", "telephony",
                                                         "S/PDIF in", "digital in", "?", "other"};
                Line l;
                l << "codec " << uint64_t(address) << " → " << kDevices[(config >> 20) & 0xF];
                str::copy(description, l.text, room);
            }
            ++paths;
        }
    }
    return paths;
}

uint32_t position(void*) {
    // The DMA position buffer (updated by the controller); LPIB when there is none.
    uint32_t p = g_hda.positions ? g_hda.positions[g_hda.stream_index * 2] : r32(g_hda.stream_base + kSdLpib);
    return p % kRingBytes;
}

bool start_stream() {
    uint32_t sd = g_hda.stream_base;
    w8(sd + kSdCtl, 1);  // stream reset
    wait_for([] { return uint32_t(r8(g_hda.stream_base + kSdCtl)); }, 1, 1, 50);
    w8(sd + kSdCtl, 0);
    wait_for([] { return uint32_t(r8(g_hda.stream_base + kSdCtl)); }, 1, 0, 50);

    g_hda.ring = static_cast<uint8_t*>(pmm::alloc_contiguous(kRingBytes / 4096));
    g_hda.bdl = static_cast<uint64_t*>(pmm::alloc_page());
    auto* positions = static_cast<uint32_t*>(pmm::alloc_page());
    if (!g_hda.ring || !g_hda.bdl || !positions) return false;
    mem::set(g_hda.ring, 0, kRingBytes);
    mem::set(positions, 0, 4096);
    uint64_t ring = pmm::virt_to_phys(g_hda.ring);
    for (uint32_t i = 0; i < kFragments; ++i) {  // address, length, flags (no interrupt on completion)
        g_hda.bdl[i * 2] = ring + uint64_t(i) * (kRingBytes / kFragments);
        g_hda.bdl[i * 2 + 1] = kRingBytes / kFragments;
    }
    uint64_t bdl = pmm::virt_to_phys(g_hda.bdl), pos = pmm::virt_to_phys(positions);
    w32(kDplLo, uint32_t(pos) | 1);
    w32(kDplHi, uint32_t(pos >> 32));
    g_hda.positions = positions;
    w32(sd + kSdBdlLo, uint32_t(bdl));
    w32(sd + kSdBdlHi, uint32_t(bdl >> 32));
    w32(sd + kSdCbl, kRingBytes);
    w16(sd + kSdLvi, kFragments - 1);
    w16(sd + kSdFmt, kFormat48k16Stereo);
    w8(sd + kSdSts, 0x1C);                          // clear status
    w8(sd + kSdCtl + 2, uint8_t(1u << 4));          // stream tag 1 (bits 23:20)
    w8(sd + kSdCtl, 2);                             // run
    return true;
}

}  // namespace

void probe_hda(uint32_t pci) {
    if (audio::available()) return;  // one output device
    g_hda.regs = static_cast<volatile uint8_t*>(pci::map_bar(pci, 0));
    if (!g_hda.regs) return fail("no register BAR");
    pci::enable(pci, true, false, true);
    if (!reset_controller()) return fail("the controller does not come out of reset");
    w32(kIntctl, 0);
    uint16_t codecs = r16(kStatests);
    w16(kStatests, codecs);
    if (!codecs) return fail("no codec answered");
    if (!start_rings()) return fail("out of memory");

    uint32_t paths = 0;
    for (uint32_t a = 0; a < 15 && paths == 0; ++a)
        if (codecs & (1u << a)) paths = setup_codec(a, g_hda.name, sizeof(g_hda.name));
    if (!paths) return fail("no codec has an output path (pin → DAC)");

    uint16_t gcap = r16(kGcap);
    uint32_t inputs = (gcap >> 8) & 0xF, outputs = (gcap >> 12) & 0xF;
    if (!outputs) return fail("no output stream engine");
    g_hda.stream_index = inputs;  // output streams come after the input ones
    g_hda.stream_base = 0x80 + 0x20 * inputs;
    if (!start_stream()) return fail("out of memory for the stream");

    audio::Backend b{"hda", g_hda.ring, kRingBytes, &position, nullptr};
    if (!audio::attach(b)) return fail("the mixer refused the device");
    term::write("[neko] hda: ");
    term::write(g_hda.name);
    term::write(", 48 kHz 16-bit stereo, /dev/audio\n");
}

}  // namespace neko::drivers
