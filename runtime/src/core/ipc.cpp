// Shared memory objects and local message channels (see neko/ipc.hpp).
//
// Both are anonymous VFS objects (vfs::open_object), so fds, dup, inheritance, close-on-exit and
// poll work for them like for any file. Locking: one spin lock per channel guards both ends; the
// listener table has its own. Files that travel inside messages are closed outside those locks.
#include <neko/ipc.hpp>
#include <neko/process.hpp>
#include <neko/runtime.hpp>
#include <neko/vfs.hpp>

namespace neko::ipc {
namespace {

using namespace proc;  // error codes

constexpr uint64_t kPage = pmm::kPageSize;

uint64_t g_shm_objects = 0, g_shm_bytes = 0, g_channels = 0, g_listeners = 0, g_messages = 0;

// ---------------------------------------------------------------- shared memory

struct Shm {
    uint64_t size;   // bytes (whole pages)
    uint64_t pages;
    void** frames;   // HHDM addresses of the frames
};

int64_t shm_read(void* context, uint64_t offset, void* buffer, uint64_t length) {
    auto* s = static_cast<Shm*>(context);
    if (offset >= s->size) return 0;
    if (length > s->size - offset) length = s->size - offset;
    auto* out = static_cast<uint8_t*>(buffer);
    for (uint64_t done = 0; done < length;) {
        uint64_t at = offset + done, in_page = kPage - at % kPage;
        uint64_t n = length - done < in_page ? length - done : in_page;
        mem::copy(out + done, static_cast<uint8_t*>(s->frames[at / kPage]) + at % kPage, n);
        done += n;
    }
    return int64_t(length);
}

int64_t shm_write(void* context, uint64_t offset, const void* buffer, uint64_t length) {
    auto* s = static_cast<Shm*>(context);
    if (offset >= s->size) return length ? kErrNoMemory : 0;  // fixed size: no growing
    if (length > s->size - offset) length = s->size - offset;
    auto* in = static_cast<const uint8_t*>(buffer);
    for (uint64_t done = 0; done < length;) {
        uint64_t at = offset + done, in_page = kPage - at % kPage;
        uint64_t n = length - done < in_page ? length - done : in_page;
        mem::copy(static_cast<uint8_t*>(s->frames[at / kPage]) + at % kPage, in + done, n);
        done += n;
    }
    return int64_t(length);
}

uint64_t shm_size(void* context) { return static_cast<Shm*>(context)->size; }

int64_t shm_map(void* context, uint64_t offset, uint64_t& phys, bool& write_combining) {
    auto* s = static_cast<Shm*>(context);
    if (offset >= s->size) return kErrBadArgument;
    phys = pmm::virt_to_phys(s->frames[offset / kPage]);
    write_combining = false;
    return 0;
}

void shm_release(void* context) {
    auto* s = static_cast<Shm*>(context);
    for (uint64_t i = 0; i < s->pages; ++i)
        if (s->frames[i]) pmm::free_page(s->frames[i]);
    __atomic_sub_fetch(&g_shm_objects, 1, __ATOMIC_RELAXED);
    __atomic_sub_fetch(&g_shm_bytes, s->size, __ATOMIC_RELAXED);
    heap::free(s->frames);
    heap::free(s);
}

const vfs::DeviceOps kShmOps = {shm_read, shm_write, shm_size, nullptr, shm_release, nullptr, nullptr, shm_map, nullptr};

// ---------------------------------------------------------------- channels

struct Message {
    Message* next;
    vfs::File* attached;
    uint32_t length;
    uint8_t data[];
};

struct Channel;

struct End {
    Channel* channel;
    uint32_t side;       // 0 or 1; the peer is the other one
    Message* head;       // messages waiting to be received here
    Message* tail;
    uint32_t queued;
    bool open;
    uint32_t waiting;    // readers asleep on `readable`
    sync::Semaphore readable;
};

struct Channel {
    sync::SpinLock lock;
    End ends[2];
    uint32_t nodes;      // VFS nodes still alive (one per end): the channel goes with the last
};

End& peer(End* e) { return e->channel->ends[1 - e->side]; }

// Takes every queued message off `e` (lock held); the caller frees them outside the lock.
Message* drain(End* e) {
    Message* m = e->head;
    e->head = e->tail = nullptr;
    e->queued = 0;
    return m;
}

void free_messages(Message* m) {
    while (m) {
        Message* next = m->next;
        vfs::close(m->attached);
        heap::free(m);
        m = next;
    }
}

void wake_readers(End& e) {
    while (e.waiting) {
        --e.waiting;
        e.readable.signal();
    }
}

int64_t recv_end(End* e, void* buffer, uint64_t capacity, vfs::File*& attached) {
    attached = nullptr;
    Channel* c = e->channel;
    for (;;) {
        c->lock.lock();
        if (Message* m = e->head) {
            e->head = m->next;
            if (!e->head) e->tail = nullptr;
            --e->queued;
            c->lock.unlock();
            vfs::notify();  // the sender may be polling for room
            uint64_t n = m->length < capacity ? m->length : capacity;
            mem::copy(buffer, m->data, n);
            attached = m->attached;
            heap::free(m);
            return int64_t(n);
        }
        if (!peer(e).open) {
            c->lock.unlock();
            return 0;
        }
        ++e->waiting;
        c->lock.unlock();
        if (!e->readable.wait_interruptible()) return kErrInterrupted;
    }
}

int64_t send_end(End* e, const void* data, uint64_t length, vfs::File* attach) {
    if (length > kMaxMessage) return kErrBadArgument;
    auto* m = static_cast<Message*>(heap::alloc(sizeof(Message) + (length ? length : 1)));
    if (!m) return kErrNoMemory;
    m->next = nullptr;
    m->length = uint32_t(length);
    mem::copy(m->data, data, length);
    m->attached = vfs::dup(attach);
    Channel* c = e->channel;
    c->lock.lock();
    End& to = peer(e);
    int64_t r = int64_t(length);
    if (!to.open) r = kErrBrokenPipe;
    else if (to.queued >= kMaxQueued) r = kErrBusy;
    if (r >= 0) {
        if (to.tail) to.tail->next = m;
        else to.head = m;
        to.tail = m;
        ++to.queued;
        wake_readers(to);
    }
    c->lock.unlock();
    if (r < 0) {
        m->next = nullptr;
        free_messages(m);
        return r;
    }
    __atomic_add_fetch(&g_messages, 1, __ATOMIC_RELAXED);
    vfs::notify();
    return r;
}

int64_t channel_read(void* context, uint64_t, void* buffer, uint64_t length) {
    vfs::File* attached = nullptr;
    int64_t r = recv_end(static_cast<End*>(context), buffer, length, attached);
    vfs::close(attached);  // plain read(): a file that came along is dropped
    return r;
}

int64_t channel_write(void* context, uint64_t, const void* buffer, uint64_t length) {
    return send_end(static_cast<End*>(context), buffer, length, nullptr);
}

uint32_t channel_poll(void* context, uint32_t) {
    auto* e = static_cast<End*>(context);
    sync::ScopedLock guard(e->channel->lock);
    End& other = peer(e);
    uint32_t r = 0;
    if (e->head || !other.open) r |= vfs::kPollIn;
    if (other.open && other.queued < kMaxQueued) r |= vfs::kPollOut;
    if (!other.open) r |= vfs::kPollHup;
    return r;
}

// The last open file of this end is gone (an end is opened exactly once; dups share that file).
void channel_close(void* context, uint32_t) {
    auto* e = static_cast<End*>(context);
    Channel* c = e->channel;
    Message* dropped;
    {
        sync::ScopedLock guard(c->lock);
        e->open = false;
        dropped = drain(e);
        wake_readers(peer(e));  // they see the end
    }
    free_messages(dropped);
    vfs::notify();
}

void channel_release(void* context) {
    auto* e = static_cast<End*>(context);
    Channel* c = e->channel;
    bool last;
    {
        sync::ScopedLock guard(c->lock);
        last = --c->nodes == 0;
    }
    if (!last) return;
    __atomic_sub_fetch(&g_channels, 1, __ATOMIC_RELAXED);
    heap::free(c);
}

const vfs::DeviceOps kChannelOps = {channel_read, channel_write, nullptr, channel_close, channel_release,
                                    nullptr, channel_poll, nullptr, nullptr};

// Both ends of a fresh channel as open files.
int64_t make_channel(vfs::File*& a, vfs::File*& b) {
    a = b = nullptr;
    auto* c = static_cast<Channel*>(heap::alloc_zeroed(sizeof(Channel)));
    if (!c) return kErrNoMemory;
    for (uint32_t i = 0; i < 2; ++i) {
        c->ends[i].channel = c;
        c->ends[i].side = i;
        c->ends[i].open = true;
    }
    c->nodes = 2;
    a = vfs::open_object("channel", &kChannelOps, &c->ends[0], vfs::kRead | vfs::kWrite);
    if (!a) {
        heap::free(c);
        return kErrNoMemory;
    }
    b = vfs::open_object("channel", &kChannelOps, &c->ends[1], vfs::kRead | vfs::kWrite);
    if (!b) {
        c->nodes = 1;   // end 1 never existed: closing end 0 frees the channel
        vfs::close(a);
        a = nullptr;
        return kErrNoMemory;
    }
    __atomic_add_fetch(&g_channels, 1, __ATOMIC_RELAXED);
    return 0;
}

// ---------------------------------------------------------------- listeners

struct Listener {
    char name[kMaxName + 1];
    bool open;
    vfs::File* pending[kMaxPending];  // server ends waiting for accept()
    uint32_t count;
    uint32_t waiting;
    sync::Semaphore arrived;
};

sync::SpinLock g_listen_lock;  // the table, and every listener's queue
Listener* g_table[kMaxListeners];

int64_t listener_read(void*, uint64_t, void*, uint64_t) { return kErrNotSupported; }

uint32_t listener_poll(void* context, uint32_t) {
    auto* l = static_cast<Listener*>(context);
    sync::ScopedLock guard(g_listen_lock);
    return l->count ? vfs::kPollIn : 0;
}

void listener_close(void* context, uint32_t) {
    auto* l = static_cast<Listener*>(context);
    vfs::File* pending[kMaxPending];
    uint32_t n;
    {
        sync::ScopedLock guard(g_listen_lock);
        for (uint32_t i = 0; i < kMaxListeners; ++i)
            if (g_table[i] == l) g_table[i] = nullptr;
        l->open = false;
        n = l->count;
        for (uint32_t i = 0; i < n; ++i) pending[i] = l->pending[i];
        l->count = 0;
        while (l->waiting) {
            --l->waiting;
            l->arrived.signal();
        }
    }
    for (uint32_t i = 0; i < n; ++i) vfs::close(pending[i]);  // their clients see the end
    vfs::notify();
}

void listener_release(void* context) {
    __atomic_sub_fetch(&g_listeners, 1, __ATOMIC_RELAXED);
    heap::free(context);
}

const vfs::DeviceOps kListenerOps = {listener_read, nullptr, nullptr, listener_close, listener_release,
                                     nullptr, listener_poll, nullptr, nullptr};

bool valid_name(const char* name) {
    uint64_t n = str::length(name);
    if (n == 0 || n > kMaxName) return false;
    for (uint64_t i = 0; i < n; ++i)
        if (name[i] <= ' ' || name[i] == '/') return false;
    return true;
}

}  // namespace

int64_t shm_create(uint64_t size, vfs::File*& out) {
    out = nullptr;
    if (size == 0 || size > kMaxShm) return kErrBadArgument;
    auto* s = static_cast<Shm*>(heap::alloc_zeroed(sizeof(Shm)));
    if (!s) return kErrNoMemory;
    s->pages = mem::align_up(size, kPage) / kPage;
    s->size = s->pages * kPage;
    s->frames = static_cast<void**>(heap::alloc_zeroed(s->pages * sizeof(void*)));
    __atomic_add_fetch(&g_shm_objects, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_shm_bytes, s->size, __ATOMIC_RELAXED);
    bool ok = s->frames != nullptr;
    for (uint64_t i = 0; ok && i < s->pages; ++i) ok = (s->frames[i] = pmm::alloc_page()) != nullptr;
    if (!ok) {
        if (s->frames) shm_release(s);
        else {
            __atomic_sub_fetch(&g_shm_objects, 1, __ATOMIC_RELAXED);
            __atomic_sub_fetch(&g_shm_bytes, s->size, __ATOMIC_RELAXED);
            heap::free(s);
        }
        return kErrNoMemory;
    }
    out = vfs::open_object("shm", &kShmOps, s, vfs::kRead | vfs::kWrite);
    if (!out) {
        shm_release(s);
        return kErrNoMemory;
    }
    return 0;
}

int64_t listen(const char* name, vfs::File*& out) {
    out = nullptr;
    if (!valid_name(name)) return kErrBadArgument;
    auto* l = static_cast<Listener*>(heap::alloc_zeroed(sizeof(Listener)));
    if (!l) return kErrNoMemory;
    str::copy(l->name, name, sizeof(l->name));
    l->open = true;
    __atomic_add_fetch(&g_listeners, 1, __ATOMIC_RELAXED);
    {
        sync::ScopedLock guard(g_listen_lock);
        int32_t slot = -1;
        for (uint32_t i = 0; i < kMaxListeners; ++i) {
            if (g_table[i] && str::equals(g_table[i]->name, name)) {
                slot = -2;
                break;
            }
            if (!g_table[i] && slot == -1) slot = int32_t(i);
        }
        if (slot < 0) {
            __atomic_sub_fetch(&g_listeners, 1, __ATOMIC_RELAXED);
            heap::free(l);
            return slot == -2 ? kErrExists : kErrLimit;
        }
        g_table[slot] = l;
    }
    out = vfs::open_object("listener", &kListenerOps, l, vfs::kRead);
    if (!out) {
        {
            sync::ScopedLock guard(g_listen_lock);
            for (uint32_t i = 0; i < kMaxListeners; ++i)
                if (g_table[i] == l) g_table[i] = nullptr;
        }
        listener_release(l);
        return kErrNoMemory;
    }
    return 0;
}

int64_t connect(const char* name, vfs::File*& out) {
    out = nullptr;
    if (!valid_name(name)) return kErrBadArgument;
    vfs::File *client, *server;
    int64_t e = make_channel(client, server);
    if (e < 0) return e;
    {
        sync::ScopedLock guard(g_listen_lock);
        Listener* l = nullptr;
        for (uint32_t i = 0; i < kMaxListeners && !l; ++i)
            if (g_table[i] && str::equals(g_table[i]->name, name)) l = g_table[i];
        if (!l) e = kErrNotFound;
        else if (l->count == kMaxPending) e = kErrBusy;
        else {
            l->pending[l->count++] = server;
            if (l->waiting) {
                --l->waiting;
                l->arrived.signal();
            }
        }
    }
    if (e < 0) {
        vfs::close(server);
        vfs::close(client);
        return e;
    }
    vfs::notify();
    out = client;
    return 0;
}

int64_t accept(vfs::File* listener, vfs::File*& out) {
    out = nullptr;
    auto* l = static_cast<Listener*>(vfs::object_context(listener, &kListenerOps));
    if (!l) return kErrBadArgument;
    for (;;) {
        g_listen_lock.lock();
        if (l->count) {
            out = l->pending[0];
            for (uint32_t i = 1; i < l->count; ++i) l->pending[i - 1] = l->pending[i];
            --l->count;
            g_listen_lock.unlock();
            return 0;
        }
        if (!l->open) {
            g_listen_lock.unlock();
            return kErrBrokenPipe;
        }
        ++l->waiting;
        g_listen_lock.unlock();
        if (!l->arrived.wait_interruptible()) return kErrInterrupted;
    }
}

int64_t send(vfs::File* channel, const void* data, uint64_t length, vfs::File* attach) {
    auto* e = static_cast<End*>(vfs::object_context(channel, &kChannelOps));
    if (!e) return kErrBadArgument;
    return send_end(e, data, length, attach);
}

int64_t recv(vfs::File* channel, void* buffer, uint64_t capacity, vfs::File*& attached) {
    attached = nullptr;
    auto* e = static_cast<End*>(vfs::object_context(channel, &kChannelOps));
    if (!e) return kErrBadArgument;
    return recv_end(e, buffer, capacity, attached);
}

Stats stats() {
    return {g_shm_objects, g_shm_bytes, g_channels, g_listeners, g_messages};
}

}  // namespace neko::ipc
