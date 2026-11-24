// Local IPC and shared memory (core/ipc.cpp) — what a display server and its clients talk through.
//
// Shared memory: an anonymous object of whole pages. Every process that has an fd of it can mmap()
// it (the same frames in each address space) and read/write it at an offset like a file. The pages
// go back to the kernel when the last fd and the last mapping are gone.
//
// Channels: message-preserving, bidirectional, between two open files. A server listen()s under a
// name ("display", "wm"…); connect(name) makes a new channel, hands one end to the caller and queues
// the other for the server's accept(). Each message is ≤ kMaxMessage bytes and may carry one open
// file (any fd: shared memory, a pipe, another channel) that the receiver gets as a new fd of its
// own — how a client hands its window buffer to the compositor. read()/write() on a channel fd are
// recv/send without a file. poll(): readable when a message is queued or the peer is gone, writable
// while the peer's queue has room.
#pragma once

#include <neko/types.hpp>

namespace neko::vfs {
struct File;
}

namespace neko::ipc {

constexpr uint32_t kMaxMessage = 4096;    // bytes of one message
constexpr uint32_t kMaxQueued = 128;      // messages waiting at one end; send fails with kErrBusy beyond
constexpr uint32_t kMaxName = 31;         // of a listening name
constexpr uint32_t kMaxListeners = 16;
constexpr uint32_t kMaxPending = 16;      // connections waiting for accept()
constexpr uint64_t kMaxShm = 64ull << 20; // bytes of one shared memory object

// ---- shared memory
int64_t shm_create(uint64_t size, vfs::File*& out);   // zeroed pages, read + write; 0 or an Error

// ---- channels (results: 0 / bytes or a negative proc::Error)
int64_t listen(const char* name, vfs::File*& out);    // kErrExists if the name is taken
int64_t connect(const char* name, vfs::File*& out);   // kErrNotFound without a listener
int64_t accept(vfs::File* listener, vfs::File*& out); // blocks (interruptible) until a connection arrives
// Never blocks: kErrBusy when the peer's queue is full, kErrBrokenPipe when the peer is gone.
// `attach` (may be null) gets an extra reference that travels with the message.
int64_t send(vfs::File* channel, const void* data, uint64_t length, vfs::File* attach);
// Blocks (interruptible) until a message arrives; 0 once the peer is gone and nothing is queued.
// A message longer than `capacity` is cut. `attached` gets the file that came with it (or nullptr).
int64_t recv(vfs::File* channel, void* buffer, uint64_t capacity, vfs::File*& attached);

struct Stats {
    uint64_t shm_objects, shm_bytes;
    uint64_t channels, listeners;
    uint64_t messages;  // sent since boot
};
Stats stats();

}  // namespace neko::ipc
