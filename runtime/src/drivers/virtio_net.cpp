// virtio-net: the network card QEMU attaches with `-device virtio-net-pci` (transport: virtio.cpp).
//
// Queue 0 receives, queue 1 transmits. Every frame is preceded by the 12-byte virtio-net header
// (all zero: no checksum offload, no segmentation — the C# stack computes its own checksums).
// Receive buffers (2 KiB each) stay posted; the interrupt handler hands each filled one to
// net::deliver and posts it again. Transmit buffers are slots taken by send() and given back when
// the device has used them. One MSI-X vector serves both queues; without MSI-X a thread polls.
#include <neko/net.hpp>
#include <neko/process.hpp>

#include "virtio.hpp"

namespace neko::drivers {
namespace {

using namespace proc;  // error codes

constexpr uint32_t kFeatureMac = 5;
constexpr uint32_t kHeader = 12;     // struct virtio_net_hdr with num_buffers (VERSION_1)
constexpr uint32_t kBuffer = 2048;   // header + the largest frame
constexpr uint16_t kQueueSize = 64;

struct Card {
    virtio::Device dev;
    virtio::Queue rx, tx;
    uint8_t* rx_buffers;  // DMA, rx.size × kBuffer
    uint8_t* tx_buffers;  // DMA, tx.size × kBuffer
    uint64_t tx_free;     // bit per transmit slot
    sync::SpinLock tx_lock;
    sync::Semaphore tx_slots;
    int32_t index;        // in net::
    bool interrupts;
};

void post_rx(Card* c, uint16_t i) {
    c->rx.desc[i] = {pmm::virt_to_phys(c->rx_buffers + uint64_t(i) * kBuffer), kBuffer, virtio::kDescWrite, 0};
    virtio::publish(c->rx, i);
}

void reap(Card* c) {
    {
        sync::ScopedLock guard(c->rx.lock);
        uint32_t head, length;
        while (virtio::next_used(c->rx, head, length)) {
            if (head >= c->rx.size) continue;
            if (length > kHeader && c->index >= 0)
                net::deliver(uint32_t(c->index), c->rx_buffers + uint64_t(head) * kBuffer + kHeader, length - kHeader);
            post_rx(c, uint16_t(head));
        }
    }
    uint32_t freed = 0;
    {
        sync::ScopedLock guard(c->tx.lock);
        uint32_t head, length;
        while (virtio::next_used(c->tx, head, length)) {
            if (head >= c->tx.size) continue;
            sync::ScopedLock slots(c->tx_lock);
            c->tx_free |= 1ull << head;
            ++freed;
        }
    }
    while (freed--) c->tx_slots.signal();
}

void on_interrupt(void* context) { reap(static_cast<Card*>(context)); }

void poll_thread(uint64_t context) {
    for (;;) {
        reap(reinterpret_cast<Card*>(context));
        sched::sleep_ms(1);
    }
}

int64_t card_send(void* context, const void* frame, uint64_t length) {
    auto* c = static_cast<Card*>(context);
    if (length + kHeader > kBuffer) return kErrBadArgument;
    c->tx_slots.wait();
    uint16_t slot;
    {
        sync::ScopedLock guard(c->tx_lock);
        slot = uint16_t(__builtin_ctzll(c->tx_free));
        c->tx_free &= ~(1ull << slot);
    }
    uint8_t* buffer = c->tx_buffers + uint64_t(slot) * kBuffer;
    mem::set(buffer, 0, kHeader);
    mem::copy(buffer + kHeader, frame, length);
    sync::ScopedLock guard(c->tx.lock);
    c->tx.desc[slot] = {pmm::virt_to_phys(buffer), uint32_t(length + kHeader), 0, 0};
    virtio::publish(c->tx, slot);
    return 0;
}

const net::Ops kOps = {card_send};
uint32_t g_cards = 0;

}  // namespace

void probe_virtio_net(uint32_t pci) {
    auto* c = static_cast<Card*>(heap::alloc_zeroed(sizeof(Card)));
    if (!c) return;
    c->index = -1;
    if (!virtio::attach(c->dev, pci)) return virtio::fail(c->dev, "virtio-net", "no virtio 1.0 capabilities (legacy-only device)");
    uint64_t features = virtio::negotiate(c->dev, 0, 1ull << kFeatureMac);
    if (!features) return virtio::fail(c->dev, "virtio-net", "features not accepted");

    uint8_t vector = c->dev.msix ? irq::allocate_msi(&on_interrupt, c, "virtio-net") : 0;
    if (!virtio::setup_queue(c->dev, c->rx, 0, kQueueSize, vector, 0) ||
        !virtio::setup_queue(c->dev, c->tx, 1, kQueueSize, vector, 1))
        return virtio::fail(c->dev, "virtio-net", "no receive/transmit queues");
    c->interrupts = c->rx.interrupts && c->tx.interrupts;

    c->rx_buffers = static_cast<uint8_t*>(pmm::alloc_contiguous(uint64_t(c->rx.size) * kBuffer / 4096));
    c->tx_buffers = static_cast<uint8_t*>(pmm::alloc_contiguous(uint64_t(c->tx.size) * kBuffer / 4096));
    if (!c->rx_buffers || !c->tx_buffers) return virtio::fail(c->dev, "virtio-net", "out of memory");
    c->tx_free = c->tx.size == 64 ? ~0ull : (1ull << c->tx.size) - 1;
    c->tx_slots.count = c->tx.size;

    uint8_t mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, uint8_t(0x56 + g_cards)};
    if (features & (1ull << kFeatureMac))
        for (uint32_t i = 0; i < 6; ++i) mac[i] = virtio::config8(c->dev, i);
    char name[8] = "eth0";
    name[3] = char('0' + g_cards++);
    c->index = net::add(name, mac, &kOps, c);
    if (c->index < 0) return virtio::fail(c->dev, "virtio-net", "interface table is full");

    {
        sync::ScopedLock guard(c->rx.lock);
        for (uint16_t i = 0; i < c->rx.size; ++i) post_rx(c, i);
    }
    virtio::driver_ok(c->dev);
    if (!c->interrupts) sched::spawn("virtio-net poll", &poll_thread, reinterpret_cast<uint64_t>(c), sched::Priority::High);

    static const char kHex[] = "0123456789abcdef";
    char text[18];
    for (uint32_t i = 0; i < 6; ++i) {
        text[i * 3] = kHex[mac[i] >> 4];
        text[i * 3 + 1] = kHex[mac[i] & 15];
        text[i * 3 + 2] = i == 5 ? 0 : ':';
    }
    term::write("[neko] virtio-net: ");
    term::write(name);
    term::write(", MAC ");
    term::write(text);
    term::write(c->interrupts ? ", MSI-X" : ", polling");
    term::write("\n");
}

}  // namespace neko::drivers
