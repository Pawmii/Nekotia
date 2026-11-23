// virtio 1.x PCI transport and split virtqueues (see virtio.hpp).
#include "virtio.hpp"

namespace neko::virtio {
namespace {

constexpr uint8_t kStatusAcknowledge = 1, kStatusDriver = 2, kStatusDriverOk = 4, kStatusFeaturesOk = 8, kStatusFailed = 128;
constexpr uint8_t kCapCommon = 1, kCapNotify = 2, kCapDevice = 4;

// Common configuration window (virtio 1.x, 4.1.4.3).
enum Common : uint32_t {
    kDeviceFeatureSelect = 0, kDeviceFeature = 4, kDriverFeatureSelect = 8, kDriverFeature = 12,
    kMsixConfig = 16, kNumQueues = 18, kDeviceStatus = 20, kConfigGeneration = 21,
    kQueueSelect = 22, kQueueSize = 24, kQueueMsixVector = 26, kQueueEnable = 28, kQueueNotifyOff = 30,
    kQueueDesc = 32, kQueueDriver = 40, kQueueDevice = 48,
};

uint8_t r8(volatile uint8_t* b, uint32_t o) { return b[o]; }
uint16_t r16(volatile uint8_t* b, uint32_t o) { return *reinterpret_cast<volatile uint16_t*>(b + o); }
uint32_t r32(volatile uint8_t* b, uint32_t o) { return *reinterpret_cast<volatile uint32_t*>(b + o); }
void w8(volatile uint8_t* b, uint32_t o, uint8_t v) { b[o] = v; }
void w16(volatile uint8_t* b, uint32_t o, uint16_t v) { *reinterpret_cast<volatile uint16_t*>(b + o) = v; }
void w32(volatile uint8_t* b, uint32_t o, uint32_t v) { *reinterpret_cast<volatile uint32_t*>(b + o) = v; }
void w64(volatile uint8_t* b, uint32_t o, uint64_t v) {
    w32(b, o, uint32_t(v));
    w32(b, o + 4, uint32_t(v >> 32));
}

void barrier() { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

}  // namespace

bool attach(Device& d, uint32_t pci) {
    d = Device{};
    d.pci = pci;
    pci::enable(pci, true, false, true);
    for (uint8_t cap = pci::find_capability(pci, 0x09); cap; cap = pci::find_capability(pci, 0x09, cap)) {
        uint8_t type = pci::read8(pci, uint16_t(cap + 3));
        uint8_t bar = pci::read8(pci, uint16_t(cap + 4));
        uint32_t offset = pci::read32(pci, uint16_t(cap + 8));
        auto* base = static_cast<volatile uint8_t*>(pci::map_bar(pci, bar));
        if (!base) continue;
        if (type == kCapCommon && !d.common) d.common = base + offset;
        else if (type == kCapNotify && !d.notify_base) {
            d.notify_base = base + offset;
            d.notify_multiplier = pci::read32(pci, uint16_t(cap + 16));
        } else if (type == kCapDevice && !d.config) d.config = base + offset;
    }
    return d.common && d.notify_base && d.config;
}

uint64_t negotiate(Device& d, uint64_t required, uint64_t optional) {
    volatile uint8_t* c = d.common;
    w8(c, kDeviceStatus, 0);  // reset
    for (int i = 0; i < 1000000 && r8(c, kDeviceStatus) != 0; ++i) cpu::pause();
    w8(c, kDeviceStatus, kStatusAcknowledge);
    w8(c, kDeviceStatus, kStatusAcknowledge | kStatusDriver);
    w32(c, kDeviceFeatureSelect, 0);
    uint64_t offered = r32(c, kDeviceFeature);
    w32(c, kDeviceFeatureSelect, 1);
    offered |= uint64_t(r32(c, kDeviceFeature)) << 32;
    required |= 1ull << kFeatureVersion1;
    if ((offered & required) != required) return 0;
    uint64_t wanted = required | (offered & optional);
    w32(c, kDriverFeatureSelect, 0);
    w32(c, kDriverFeature, uint32_t(wanted));
    w32(c, kDriverFeatureSelect, 1);
    w32(c, kDriverFeature, uint32_t(wanted >> 32));
    w8(c, kDeviceStatus, kStatusAcknowledge | kStatusDriver | kStatusFeaturesOk);
    if (!(r8(c, kDeviceStatus) & kStatusFeaturesOk)) return 0;
    w16(c, kMsixConfig, kNoVector);  // no configuration-change interrupts
    d.msix = pci::msix_enable(d.pci) > 0;
    return wanted;
}

bool setup_queue(Device& d, Queue& q, uint16_t index, uint16_t max_size, uint8_t vector, uint16_t entry) {
    volatile uint8_t* c = d.common;
    w16(c, kQueueSelect, index);
    uint16_t max = r16(c, kQueueSize);
    if (max == 0) return false;
    uint16_t size = max_size > 64 ? 64 : max_size;
    while (size > max) size /= 2;
    auto* ring = static_cast<uint8_t*>(pmm::alloc_page());
    if (!ring) return false;
    q.index = index;
    q.size = size;
    q.desc = reinterpret_cast<Desc*>(ring);
    q.avail = reinterpret_cast<volatile uint16_t*>(ring + 16 * size);
    q.used = ring + 2048;
    q.last_used = 0;
    q.avail_idx = 0;
    w16(c, kQueueSize, size);
    w64(c, kQueueDesc, pmm::virt_to_phys(ring));
    w64(c, kQueueDriver, pmm::virt_to_phys(ring + 16 * size));
    w64(c, kQueueDevice, pmm::virt_to_phys(ring + 2048));
    uint16_t routed = kNoVector;
    if (vector && d.msix && pci::msix_route(d.pci, entry, vector)) {
        w16(c, kQueueMsixVector, entry);
        routed = r16(c, kQueueMsixVector);  // 0xFFFF when the device cannot
    }
    if (routed == kNoVector) w16(c, kQueueMsixVector, kNoVector);
    q.interrupts = routed != kNoVector;
    uint16_t off = r16(c, kQueueNotifyOff);
    q.notify = reinterpret_cast<volatile uint16_t*>(d.notify_base + uint32_t(off) * d.notify_multiplier);
    w16(c, kQueueEnable, 1);
    return true;
}

void driver_ok(Device& d) {
    w8(d.common, kDeviceStatus, kStatusAcknowledge | kStatusDriver | kStatusFeaturesOk | kStatusDriverOk);
}

void fail(Device& d, const char* driver, const char* why) {
    if (d.common) w8(d.common, kDeviceStatus, kStatusFailed);
    term::write("[neko] ");
    term::write(driver);
    term::write(": \x1b[93m");
    term::write(why);
    term::write("\x1b[0m — device ignored\n");
}

void publish(Queue& q, uint16_t head) {
    q.avail[2 + q.avail_idx % q.size] = head;
    barrier();
    ++q.avail_idx;
    q.avail[1] = q.avail_idx;
    barrier();
    *q.notify = q.index;
}

bool next_used(Queue& q, uint32_t& head, uint32_t& length) {
    uint16_t idx = *reinterpret_cast<volatile uint16_t*>(q.used + 2);
    if (q.last_used == idx) return false;
    barrier();
    volatile uint8_t* e = q.used + 4 + (q.last_used % q.size) * 8;
    head = *reinterpret_cast<volatile uint32_t*>(e);
    length = *reinterpret_cast<volatile uint32_t*>(e + 4);
    ++q.last_used;
    return true;
}

uint8_t config8(Device& d, uint32_t o) { return r8(d.config, o); }
uint16_t config16(Device& d, uint32_t o) { return r16(d.config, o); }
uint32_t config32(Device& d, uint32_t o) { return r32(d.config, o); }
uint64_t config64(Device& d, uint32_t o) { return uint64_t(r32(d.config, o)) | (uint64_t(r32(d.config, o + 4)) << 32); }

}  // namespace neko::virtio
