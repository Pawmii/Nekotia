// Virtual file system (core/vfs.cpp): one tree of nodes for the whole system.
//
//   /             ramfs: files and directories live in the kernel heap
//   /bin, /etc …  unpacked from the initrd (a ustar archive the bootloader loads as a module);
//                 its files point straight into the module until the first write copies them
//   /dev          devices: console (keyboard + screen), null, zero
//   /tmp          scratch space
//
// Paths are absolute or relative to the caller's working directory (per process; kernel threads
// share one). Open files are reference counted: a removed file lives on until its last close.
// Errors are the negative neko::proc::Error values.
#pragma once

#include <neko/types.hpp>

namespace neko::vfs {

enum class Type : uint8_t { None = 0, File = 1, Directory = 2, Device = 3 };

// open() flags
constexpr uint32_t kRead = 1;
constexpr uint32_t kWrite = 2;
constexpr uint32_t kCreate = 4;     // create the file if it does not exist
constexpr uint32_t kTruncate = 8;   // empty it on open (needs kWrite)
constexpr uint32_t kAppend = 16;    // every write goes to the end
constexpr uint32_t kExclusive = 32; // with kCreate: fail with kErrExists if it is there

// seek() origins
constexpr int32_t kSeekSet = 0, kSeekCurrent = 1, kSeekEnd = 2;

constexpr uint32_t kMaxName = 63;    // bytes of one path component
constexpr uint32_t kMaxPath = 256;

struct Node;
struct File;

// Field names match sdk/Nekotia.Sdk/IO/Vfs.cs and sdk/Nekotia.User/File.cs.
struct Stat {
    Type type;
    uint8_t reserved[3];
    uint32_t id;           // node number, unique while the node exists
    uint64_t size;         // bytes (files), entries (directories)
    uint64_t created_ms;   // uptime when it was created
    uint64_t modified_ms;  // uptime of the last write
};

struct DirEntry {
    uint8_t name[kMaxName + 1];
    Type type;
    uint8_t reserved[7];
    uint64_t size;
};

// A device node: calls instead of stored bytes. Called without the VFS lock (they may block).
// With `size`, the device is seekable (a disk): reads and writes move the file offset.
// `close` runs when an open file of the device goes away (with its open flags), `release` when the
// node itself does; both are optional and must not block.
struct DeviceOps {
    int64_t (*read)(void* context, uint64_t offset, void* buffer, uint64_t length);
    int64_t (*write)(void* context, uint64_t offset, const void* buffer, uint64_t length);
    uint64_t (*size)(void* context);
    void (*close)(void* context, uint32_t flags);
    void (*release)(void* context);
};

void init();  // the root, /dev, /tmp, /bin, /etc; needs the heap

// Bootloader modules: a ustar archive is unpacked into /, anything else becomes /bin/NAME.
void add_module(const char* path, const void* data, uint64_t size);
// Unpacks a ustar archive below `directory`; files keep pointing into `data`. Number of entries or Error.
int64_t unpack_tar(const char* directory, const void* data, uint64_t size);

// ---- files
int64_t open(const char* path, uint32_t flags, File*& out);
inline int64_t open(const uint8_t* path, uint32_t flags, File*& out) { return open(reinterpret_cast<const char*>(path), flags, out); }
int64_t read(File* f, void* buffer, uint64_t length);         // bytes read, 0 at the end
int64_t write(File* f, const void* buffer, uint64_t length);  // bytes written
int64_t seek(File* f, int64_t offset, int32_t whence);        // new offset
int64_t truncate(File* f, uint64_t size);
int64_t fstat(File* f, Stat& out);
// Entry number `index` of an open directory: 1, 0 past the end, or an Error.
int64_t readdir(File* f, uint32_t index, DirEntry& out);
File* dup(File* f);  // one more reference to the same open file (shared offset)
// A pipe: what is written to `write_end` comes out of `read_end` (64 KiB buffer). Reads block while it is
// empty and return 0 once every write end is closed; writes block while it is full and fail with
// kErrBrokenPipe once every read end is closed. Both waits end early when the caller is killed.
int64_t pipe(File*& read_end, File*& write_end);

// Sockets: open files whose reads, writes and last close go to the network stack (C#, kernel/Net).
// The stack registers its three functions once; `context` identifies the socket. The close function
// must not block (it may run while a process is being torn down).
using SocketIo = int64_t (*)(void* context, uint8_t* buffer, uint64_t length);
using SocketClose = void (*)(void* context);
void set_socket_callbacks(SocketIo read, SocketIo write, SocketClose close);
File* open_socket(void* context);   // read + write; nullptr when out of memory
void* socket_context(File* f);      // the context of a socket file, nullptr for anything else
void close(File* f);
uint32_t flags(File* f);

// ---- names
int64_t stat(const char* path, Stat& out);
inline int64_t stat(const uint8_t* path, Stat& out) { return stat(reinterpret_cast<const char*>(path), out); }
int64_t mkdir(const char* path);
inline int64_t mkdir(const uint8_t* path) { return mkdir(reinterpret_cast<const char*>(path)); }
int64_t remove(const char* path);  // a file, or an empty directory
inline int64_t remove(const uint8_t* path) { return remove(reinterpret_cast<const char*>(path)); }
int64_t rename(const char* from, const char* to);  // also moves between directories
inline int64_t rename(const uint8_t* from, const uint8_t* to) { return rename(reinterpret_cast<const char*>(from), reinterpret_cast<const char*>(to)); }
int64_t create_device(const char* path, const DeviceOps* ops, void* context);
// A generated file (/proc): every read calls `generate` to write the whole text (≤ 64 KiB) and returns
// the part at the file offset. Generators run without the VFS lock and may take other locks.
using Generator = int64_t (*)(uint8_t* buffer, uint64_t capacity);
int64_t create_generated(const char* path, Generator generate);
inline int64_t create_generated(const uint8_t* path, Generator generate) { return create_generated(reinterpret_cast<const char*>(path), generate); }

// Whole file into a fresh heap block (caller frees with heap::free). Size or Error.
int64_t read_all(const char* path, uint8_t*& data, uint64_t max_size);

// ---- working directory (of the current process; kernel threads share one)
int64_t chdir(const char* path);
inline int64_t chdir(const uint8_t* path) { return chdir(reinterpret_cast<const char*>(path)); }
int64_t getcwd(char* buffer, uint64_t capacity);  // length or Error
inline int64_t getcwd(uint8_t* buffer, uint64_t capacity) { return getcwd(reinterpret_cast<char*>(buffer), capacity); }
// For processes: a reference to the directory a new process starts in, and dropping it again.
Node* cwd_retain();
void node_release(Node* n);

// ---- the console device (/dev/console): writes go to the terminal, reads to this function
// (the keyboard line discipline in C#, kernel/Syscalls.neko). It returns bytes, 0 at end of
// input, or kErrInterrupted when the reading program is being killed.
using ConsoleReader = int64_t (*)(uint8_t* buffer, uint64_t length);
void set_console_reader(ConsoleReader reader);
File* open_console();  // a fresh open file for fds 0, 1, 2 of processes the kernel starts

// ---- mounted file systems
//
// A driver (fat.cpp) mounts a volume on an empty directory and builds the whole directory tree as
// VFS nodes at mount time (names are in memory, so path walks never touch the disk). File contents
// and every change to the tree go through FsOps, called WITHOUT the VFS lock (they do I/O) — the
// driver serialises its volume with its own mutex and updates the tree through the helpers below.
// release() runs when a node's memory goes and must not block.
struct FsOps {
    const char* type;  // "fat32"
    int64_t (*read)(void* fs, Node* n, uint64_t offset, void* buffer, uint64_t length);
    int64_t (*write)(void* fs, Node* n, uint64_t offset, const void* buffer, uint64_t length);
    int64_t (*truncate)(void* fs, Node* n, uint64_t size);
    // Creates `name` in `dir` on disk and in the tree (add_node with hold = true): out has an extra reference.
    int64_t (*create)(void* fs, Node* dir, const char* name, Type type, Node*& out);
    int64_t (*remove)(void* fs, Node* n);  // on disk, then detach_node; directories are empty
    int64_t (*rename)(void* fs, Node* n, Node* dir, const char* name);  // on disk, then move_node; `name` is free
    int64_t (*sync)(void* fs);
    void (*release)(void* fs, void* fs_data);
    void (*unmount)(void* fs);  // after a sync, once every node is gone: free the volume
    void (*usage)(void* fs, uint64_t& total_bytes, uint64_t& free_bytes);
};

// Mounts `fs` on the empty directory `dir`; root_data becomes the directory's fs_data. Out: that node.
int64_t mount(const char* dir, const FsOps* ops, void* fs, const char* source, void* root_data, Node*& root);
inline int64_t mount(const uint8_t* dir, const FsOps* ops, void* fs, const char* source, void* root_data, Node*& root) {
    return mount(reinterpret_cast<const char*>(dir), ops, fs, source, root_data, root);
}
int64_t unmount(const char* dir);  // kErrBusy while anything below it is open or a working directory
inline int64_t unmount(const uint8_t* dir) { return unmount(reinterpret_cast<const char*>(dir)); }
int64_t sync_all();                // every mounted volume

// Field names match sdk/Nekotia.Sdk/IO/Vfs.cs.
struct MountInfo {
    uint8_t path[64];
    uint8_t source[16];  // device, e.g. "vda"
    uint8_t type[8];
    uint64_t total_bytes;
    uint64_t free_bytes;
};
bool mount_info(uint32_t index, MountInfo& out);

// Tree helpers for drivers (they take the VFS lock themselves).
Node* add_node(Node* dir, const char* name, Type type, uint64_t size, void* fs_data, bool hold);
void detach_node(Node* n);
void move_node(Node* n, Node* dir, const char* name);
void set_size(Node* n, uint64_t size);
uint64_t node_size(Node* n);
void* node_data(Node* n);
bool has_children(Node* n);
bool has_child(Node* dir, const char* name);

// ---- statistics
struct Stats {
    uint64_t nodes;
    uint64_t open_files;
    uint64_t heap_bytes;      // file contents in the heap
    uint64_t initrd_bytes;    // file contents still pointing into the initrd module
    uint64_t initrd_entries;  // unpacked at boot
};
Stats stats();

}  // namespace neko::vfs
