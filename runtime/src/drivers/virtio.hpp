// virtio 1.x over PCI ("modern" transport), shared by virtio_blk.cpp and virtio_net.cpp.
//
//   PCI vendor capabilities → common / notify / device configuration windows in a BAR
//   reset → ACKNOWLEDGE → DRIVER → features → FEATURES_OK → queues → DRIVER_OK
//
// Split virtqueues: descriptor table, avail ring and used ring in one zeroed page (≤ 64 entries).
#pragma once

#include <neko/runtime.hpp>

namespace neko::virtio {

constexpr uint16_t kDescNext = 1, kDescWrite = 2;
constexpr uint32_t kFeatureVersion1 = 32;
constexpr uint16_t kNoVector = 0xFFFF;

struct Desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct Device {
    uint32_t pci;
    volatile uint8_t* common;
    volatile uint8_t* notify_base;
    uint32_t notify_multiplier;
    volatile uint8_t* config;  // device-specific configuration
    bool msix;                 // MSI-X table enabled (queues can be routed to vectors)
};

struct Queue {
    uint16_t index;
    uint16_t size;
    Desc* desc;
    volatile uint16_t* avail;  // flags, idx, ring[size]
    volatile uint8_t* used;    // flags, idx, ring[size] {id, len}
    volatile uint16_t* notify;
    uint16_t last_used;
    uint16_t avail_idx;
    bool interrupts;  // its MSI-X vector is live (otherwise the driver polls)
    sync::SpinLock lock;
};

// Finds the configuration windows and enables memory decoding + bus mastering. False: not a modern device.
bool attach(Device& d, uint32_t pci);
// Reset, ACKNOWLEDGE, DRIVER, then features: `required` must be offered (VERSION_1 is always required),
// `optional` is taken when offered. Returns the accepted set, or 0 on failure (device marked FAILED).
uint64_t negotiate(Device& d, uint64_t required, uint64_t optional);
// Sets up queue `index` with at most `max_size` entries. With an MSI-X `vector` (from irq::allocate_msi),
// MSI-X table entry `entry` is pointed at it and the queue raises it; vector 0 = no interrupt (poll).
// Returns false when out of memory or the queue does not exist.
bool setup_queue(Device& d, Queue& q, uint16_t index, uint16_t max_size, uint8_t vector, uint16_t entry);
void driver_ok(Device& d);
void fail(Device& d, const char* driver, const char* why);  // marks FAILED and logs

// Queue operations (q.lock held by the caller).
void publish(Queue& q, uint16_t head);                  // head of a ready chain → avail ring, then notify
bool next_used(Queue& q, uint32_t& head, uint32_t& length);  // one finished chain, false when none

uint8_t config8(Device& d, uint32_t offset);
uint16_t config16(Device& d, uint32_t offset);
uint32_t config32(Device& d, uint32_t offset);
uint64_t config64(Device& d, uint32_t offset);

}  // namespace neko::virtio
