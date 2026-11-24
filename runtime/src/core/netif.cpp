// Network interface registry and receive rings (see neko/net.hpp). Drivers hand received frames to
// deliver() — usually from their interrupt handler — which copies them into a per-interface ring
// and wakes the stack's receive thread.
#include <neko/net.hpp>
#include <neko/process.hpp>
#include <neko/runtime.hpp>

namespace neko::net {
namespace {

using namespace proc;  // error codes

constexpr uint32_t kRing = 128;  // frames waiting for the stack

struct Frame {
    uint16_t length;
    uint8_t data[kMaxFrame];
};

struct Interface {
    Info info;
    const Ops* ops;
    void* context;
    Frame* ring;
    uint32_t head, count;
    sync::SpinLock lock;
    sync::Semaphore ready;
};

Interface g_ifs[kMaxInterfaces];
uint32_t g_count = 0;

Interface* get(uint32_t i) { return i < __atomic_load_n(&g_count, __ATOMIC_ACQUIRE) ? &g_ifs[i] : nullptr; }

}  // namespace

int32_t add(const char* name, const uint8_t* mac, const Ops* ops, void* context) {
    if (!ops || !ops->send || g_count == kMaxInterfaces) return kErrLimit;
    auto* ring = static_cast<Frame*>(heap::alloc(sizeof(Frame) * kRing));
    if (!ring) return kErrNoMemory;
    uint32_t index = g_count;
    Interface& f = g_ifs[index];
    mem::set(&f.info, 0, sizeof(f.info));
    str::copy(reinterpret_cast<char*>(f.info.name), name, sizeof(f.info.name));
    mem::copy(f.info.mac, mac, 6);
    f.info.mtu = 1500;
    f.ops = ops;
    f.context = context;
    f.ring = ring;
    __atomic_store_n(&g_count, index + 1, __ATOMIC_RELEASE);
    return int32_t(index);
}

void deliver(uint32_t index, const void* frame, uint64_t length) {
    Interface* f = get(index);
    if (!f || length < 14 || length > kMaxFrame) return;
    {
        sync::ScopedLock guard(f->lock);
        if (f->count == kRing) {
            ++f->info.rx_dropped;
            return;
        }
        Frame& slot = f->ring[(f->head + f->count) % kRing];
        slot.length = uint16_t(length);
        mem::copy(slot.data, frame, length);
        ++f->count;
        ++f->info.rx_packets;
        f->info.rx_bytes += length;
    }
    f->ready.signal();
}

uint32_t count() { return __atomic_load_n(&g_count, __ATOMIC_ACQUIRE); }

bool info(uint32_t index, Info& out) {
    Interface* f = get(index);
    if (!f) return false;
    sync::ScopedLock guard(f->lock);
    out = f->info;
    return true;
}

int64_t send(uint32_t index, const void* frame, uint64_t length) {
    Interface* f = get(index);
    if (!f) return kErrNotFound;
    if (length < 14 || length > kMaxFrame) return kErrBadArgument;
    int64_t r = f->ops->send(f->context, frame, length);
    if (r == 0) {
        sync::ScopedLock guard(f->lock);
        ++f->info.tx_packets;
        f->info.tx_bytes += length;
    }
    return r;
}

int64_t receive(uint32_t index, void* buffer, uint64_t capacity, uint64_t timeout_ms) {
    Interface* f = get(index);
    if (!f) return kErrNotFound;
    if (timeout_ms == 0) {
        f->ready.wait();
    } else if (f->ready.wait_for(timeout_ms, false) <= 0) {
        return 0;
    }
    sync::ScopedLock guard(f->lock);
    if (f->count == 0) return 0;
    Frame& slot = f->ring[f->head];
    uint64_t n = slot.length < capacity ? slot.length : capacity;
    mem::copy(buffer, slot.data, n);
    f->head = (f->head + 1) % kRing;
    --f->count;
    return int64_t(n);
}

}  // namespace neko::net
