// Network interfaces (core/netif.cpp): Ethernet frames in and out, whatever the driver behind them
// (virtio-net in drivers/virtio_net.cpp). The protocol stack itself — ARP, IPv4, ICMP, UDP, DHCP,
// TCP — is C# (kernel/Net/), on top of send() and receive().
#pragma once

#include <neko/types.hpp>

namespace neko::net {

constexpr uint32_t kMaxInterfaces = 4;
constexpr uint32_t kMaxFrame = 1514;  // Ethernet without the FCS

struct Ops {
    // Queues one frame (copied); 0 or an Error. Blocks while the device's transmit ring is full.
    int64_t (*send)(void* context, const void* frame, uint64_t length);
};

// Field names match sdk/Nekotia.Sdk/Net/NetDevice.cs.
struct Info {
    uint8_t name[8];
    uint8_t mac[6];
    uint16_t mtu;
    uint64_t rx_packets, tx_packets, rx_bytes, tx_bytes, rx_dropped;
};

// Driver side.
int32_t add(const char* name, const uint8_t* mac, const Ops* ops, void* context);  // index or Error
// A received frame (copied; callable from interrupt handlers). Dropped when the receive ring is full.
void deliver(uint32_t index, const void* frame, uint64_t length);

// Stack side.
uint32_t count();
bool info(uint32_t index, Info& out);
int64_t send(uint32_t index, const void* frame, uint64_t length);
// Waits up to `timeout_ms` (0 = forever) for a frame; its length, 0 on timeout, or an Error.
int64_t receive(uint32_t index, void* buffer, uint64_t capacity, uint64_t timeout_ms);

}  // namespace neko::net
