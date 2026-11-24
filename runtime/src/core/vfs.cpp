// The virtual file system: a tree of nodes in the kernel heap (ramfs), device nodes, the initrd
// unpacker and open files (see neko/vfs.hpp).
//
// Locking: one spin lock guards the whole tree, every node and every open file's offset. It is never
// held across a device call (those may block, e.g. the console waiting for a line) or a user-memory
// access; the open file keeps its node alive meanwhile. Copies under the lock are bounded by the
// callers (system calls move at most 64 KiB per call).
//
// Lifetime: a node holds one reference while it is linked into the tree, plus one per open file and
// per working directory. remove() unlinks it; the memory goes when the last reference does.
#include <neko/process.hpp>
#include <neko/runtime.hpp>
#include <neko/vfs.hpp>

namespace neko::vfs {

struct Node {
    char name[kMaxName + 1];
    Type type;
    bool linked;    // reachable from the root
    bool borrowed;  // `data` points into the initrd module: not ours to free or to write
    uint32_t id;
    uint32_t refs;
    Node* parent;
    Node* children;  // directories, in creation order
    Node* next;      // sibling
    uint32_t child_count;
    uint8_t* data;
    uint64_t size;
    uint64_t capacity;
    const DeviceOps* ops;
    void* context;
    uint64_t created_ms;
    uint64_t modified_ms;
    // Nodes of a mounted volume: the driver, its volume and its per-node data.
    const FsOps* fs_ops;
    void* fs;
    void* fs_data;
    bool mount_root;
};

struct File {
    Node* node;
    uint64_t offset;
    uint32_t flags;
    uint32_t refs;
};

namespace {

using namespace proc;  // error codes

sync::SpinLock g_lock;
Node* g_root = nullptr;
Node* g_kernel_cwd = nullptr;  // shared by every kernel thread
Node* g_console = nullptr;
uint32_t g_next_id = 1;
ConsoleReader g_console_reader = nullptr;

uint64_t g_nodes = 0;
uint64_t g_open_files = 0;
uint64_t g_heap_bytes = 0;
uint64_t g_initrd_bytes = 0;
uint64_t g_initrd_entries = 0;

struct Mount {
    Node* root;
    const FsOps* ops;
    void* fs;
    char source[16];
};
constexpr uint32_t kMaxMounts = 8;
Mount g_mounts[kMaxMounts];

struct Locked {
    Locked() { g_lock.lock(); }
    ~Locked() { g_lock.unlock(); }
};

// ---------------------------------------------------------------- nodes (g_lock held)

Node* new_node(const char* name, uint32_t length, Type type) {
    auto* n = static_cast<Node*>(heap::alloc_zeroed(sizeof(Node)));
    if (!n) return nullptr;
    mem::copy(n->name, name, length);
    n->name[length] = 0;
    n->type = type;
    n->id = g_next_id++;
    n->refs = 1;
    n->created_ms = n->modified_ms = time::uptime_ms();
    ++g_nodes;
    return n;
}

void drop_data(Node* n) {
    if (n->borrowed) g_initrd_bytes -= n->size;
    else if (n->data) {
        g_heap_bytes -= n->capacity;
        heap::free(n->data);
    }
    n->data = nullptr;
    n->borrowed = false;
    n->size = n->capacity = 0;
}

void release(Node* n) {
    if (--n->refs > 0) return;
    drop_data(n);
    if (n->fs_ops && n->fs_data && !n->mount_root) n->fs_ops->release(n->fs, n->fs_data);
    if (n->type == Type::Device && n->ops->release) n->ops->release(n->context);
    heap::free(n);
    --g_nodes;
}

void attach(Node* dir, Node* n) {
    n->fs_ops = dir->fs_ops;  // a node belongs to the volume of its directory
    n->fs = dir->fs;
    n->parent = dir;
    n->next = nullptr;
    n->linked = true;
    Node** tail = &dir->children;
    while (*tail) tail = &(*tail)->next;
    *tail = n;
    ++dir->child_count;
    dir->modified_ms = time::uptime_ms();
}

void detach(Node* n) {  // keeps the tree's reference: the caller releases or re-attaches
    Node* dir = n->parent;
    for (Node** pp = &dir->children; *pp; pp = &(*pp)->next) {
        if (*pp == n) {
            *pp = n->next;
            break;
        }
    }
    --dir->child_count;
    dir->modified_ms = time::uptime_ms();
    n->parent = nullptr;
    n->next = nullptr;
    n->linked = false;
}

Node* child(Node* dir, const char* name, uint32_t length) {
    for (Node* c = dir->children; c; c = c->next)
        if (str::length(c->name) == length && mem::compare(c->name, name, length) == 0) return c;
    return nullptr;
}

Node* cwd() {
    proc::Process* p = proc::current();
    return p && p->cwd ? p->cwd : g_kernel_cwd;
}

// Next component of `p` (skipping slashes): start and length; false at the end.
bool component(const char*& p, const char*& start, uint32_t& length) {
    while (*p == '/') ++p;
    if (!*p) return false;
    start = p;
    while (*p && *p != '/') ++p;
    length = uint32_t(p - start);
    return true;
}

bool is_dot(const char* s, uint32_t n) { return n == 1 && s[0] == '.'; }
bool is_dotdot(const char* s, uint32_t n) { return n == 2 && s[0] == '.' && s[1] == '.'; }

int64_t check_path(const char* path) {
    if (!path || !*path) return kErrNotFound;
    uint64_t n = str::length(path);
    if (n >= kMaxPath) return kErrNameTooLong;
    return 0;
}

Node* start_of(const char* path) { return path[0] == '/' ? g_root : cwd(); }

// The node `path` names.
int64_t walk(const char* path, Node*& out) {
    if (int64_t e = check_path(path)) return e;
    Node* n = start_of(path);
    if (!n->linked && n != g_root) return kErrNotFound;  // the working directory was removed
    const char *p = path, *s;
    uint32_t len;
    while (component(p, s, len)) {
        if (len > kMaxName) return kErrNameTooLong;
        if (n->type != Type::Directory) return kErrNotDirectory;
        if (is_dot(s, len)) continue;
        if (is_dotdot(s, len)) {
            if (n->parent) n = n->parent;
            continue;
        }
        Node* c = child(n, s, len);
        if (!c) return kErrNotFound;
        n = c;
    }
    out = n;
    return 0;
}

// The directory that holds the last component of `path`, and that component.
int64_t walk_parent(const char* path, Node*& dir, char* name) {
    if (int64_t e = check_path(path)) return e;
    Node* n = start_of(path);
    if (!n->linked && n != g_root) return kErrNotFound;
    const char *p = path, *s, *last = nullptr;
    uint32_t len, last_len = 0;
    while (component(p, s, len)) {
        if (len > kMaxName) return kErrNameTooLong;
        if (last) {  // descend into the previous component
            if (n->type != Type::Directory) return kErrNotDirectory;
            if (is_dotdot(last, last_len)) {
                if (n->parent) n = n->parent;
            } else if (!is_dot(last, last_len)) {
                Node* c = child(n, last, last_len);
                if (!c) return kErrNotFound;
                n = c;
            }
        }
        last = s;
        last_len = len;
    }
    if (!last || is_dot(last, last_len) || is_dotdot(last, last_len)) return kErrBadArgument;
    if (n->type != Type::Directory) return kErrNotDirectory;
    mem::copy(name, last, last_len);
    name[last_len] = 0;
    dir = n;
    return 0;
}

// Creates `name` in `dir` (which must not have it yet).
int64_t make(Node* dir, const char* name, Type type, Node*& out) {
    if (!dir->linked) return kErrNotFound;
    Node* n = new_node(name, uint32_t(str::length(name)), type);
    if (!n) return kErrNoMemory;
    attach(dir, n);
    out = n;
    return 0;
}

// Room for `size` bytes of our own (copies borrowed initrd data first).
bool reserve(Node* n, uint64_t size) {
    if (n->borrowed) {
        uint64_t cap = n->size < 64 ? 64 : n->size;
        if (cap < size) cap = size;
        auto* copy = static_cast<uint8_t*>(heap::alloc(cap));
        if (!copy) return false;
        mem::copy(copy, n->data, n->size);
        g_initrd_bytes -= n->size;
        g_heap_bytes += cap;
        n->data = copy;
        n->capacity = cap;
        n->borrowed = false;
    }
    if (size <= n->capacity) return true;
    uint64_t cap = n->capacity ? n->capacity : 64;
    while (cap < size) cap *= 2;
    auto* grown = static_cast<uint8_t*>(heap::realloc(n->data, cap));
    if (!grown) return false;
    g_heap_bytes += cap - n->capacity;
    n->data = grown;
    n->capacity = cap;
    return true;
}

int64_t resize(Node* n, uint64_t size) {
    if (size == n->size) return 0;
    if (size == 0) {
        drop_data(n);
    } else {
        if (!reserve(n, size)) return kErrNoMemory;
        if (size > n->size) mem::set(n->data + n->size, 0, size - n->size);
        n->size = size;
    }
    n->modified_ms = time::uptime_ms();
    return 0;
}

int64_t generated_read(void* context, uint64_t offset, void* buffer, uint64_t length);

// Under the VFS lock: a generated file reports 0 (its generator may take sleeping locks).
uint64_t size_of(const Node* n) {
    if (n->type == Type::Directory) return n->child_count;
    if (n->type == Type::Device && n->ops->read == generated_read) return 0;
    if (n->type == Type::Device) return n->ops->size ? n->ops->size(n->context) : 0;
    return n->size;
}

void fill_stat(const Node* n, Stat& out) {
    mem::set(&out, 0, sizeof(out));
    out.type = n->type;
    out.id = n->id;
    out.size = size_of(n);
    out.created_ms = n->created_ms;
    out.modified_ms = n->modified_ms;
}

File* new_file(Node* n, uint32_t flags) {
    auto* f = static_cast<File*>(heap::alloc_zeroed(sizeof(File)));
    if (!f) return nullptr;
    f->node = n;
    f->flags = flags;
    f->refs = 1;
    ++n->refs;
    ++g_open_files;
    return f;
}

// ---------------------------------------------------------------- devices

int64_t console_read(void*, uint64_t, void* buffer, uint64_t length) {
    ConsoleReader reader = __atomic_load_n(&g_console_reader, __ATOMIC_ACQUIRE);
    return reader ? reader(static_cast<uint8_t*>(buffer), length) : 0;
}

int64_t console_write(void*, uint64_t, const void* buffer, uint64_t length) {
    auto* src = static_cast<const char*>(buffer);
    char chunk[257];
    for (uint64_t done = 0; done < length;) {
        uint64_t n = length - done < 256 ? length - done : 256;
        mem::copy(chunk, src + done, n);
        chunk[n] = 0;
        // A NUL inside the data would end term::write early: print the rest separately.
        for (uint64_t i = 0; i < n;) {
            term::write(chunk + i);
            i += str::length(chunk + i) + 1;
        }
        done += n;
    }
    return int64_t(length);
}

int64_t null_read(void*, uint64_t, void*, uint64_t) { return 0; }
int64_t null_write(void*, uint64_t, const void*, uint64_t length) { return int64_t(length); }
int64_t zero_read(void*, uint64_t, void* buffer, uint64_t length) {
    mem::set(buffer, 0, length);
    return int64_t(length);
}

const DeviceOps kConsoleOps = {console_read, console_write, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
const DeviceOps kNullOps = {null_read, null_write, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
const DeviceOps kZeroOps = {zero_read, null_write, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};

// ---------------------------------------------------------------- pipes

constexpr uint32_t kPipeSize = 64 * 1024;

struct Pipe {
    sync::SpinLock lock;
    uint8_t* data;
    uint32_t head;   // next byte to read
    uint32_t count;  // bytes buffered
    uint32_t readers, writers;  // open ends
    uint32_t waiting_readers, waiting_writers;
    sync::Semaphore readable, writable;  // "something changed" for the waiters
};

// Wakes everyone waiting on one side (pipe lock held); they re-check and wait again if needed.
void wake(sync::Semaphore& s, uint32_t& waiting) {
    while (waiting) {
        --waiting;
        s.signal();
    }
}

int64_t pipe_read(void* context, uint64_t, void* buffer, uint64_t length) {
    auto* p = static_cast<Pipe*>(context);
    auto* out = static_cast<uint8_t*>(buffer);
    if (length == 0) return 0;
    for (;;) {
        p->lock.lock();
        if (p->count > 0) {
            uint64_t n = p->count < length ? p->count : length;
            for (uint64_t i = 0; i < n; ++i) out[i] = p->data[(p->head + i) % kPipeSize];
            p->head = uint32_t((p->head + n) % kPipeSize);
            p->count -= uint32_t(n);
            wake(p->writable, p->waiting_writers);
            p->lock.unlock();
            notify();  // writers polling for space
            return int64_t(n);
        }
        if (p->writers == 0) {
            p->lock.unlock();
            return 0;  // end of input
        }
        ++p->waiting_readers;
        p->lock.unlock();
        if (!p->readable.wait_interruptible()) return kErrInterrupted;
    }
}

int64_t pipe_write(void* context, uint64_t, const void* buffer, uint64_t length) {
    auto* p = static_cast<Pipe*>(context);
    auto* in = static_cast<const uint8_t*>(buffer);
    uint64_t done = 0;
    while (done < length) {
        p->lock.lock();
        if (p->readers == 0) {
            p->lock.unlock();
            return kErrBrokenPipe;
        }
        if (p->count < kPipeSize) {
            uint64_t n = kPipeSize - p->count;
            if (n > length - done) n = length - done;
            uint32_t tail = (p->head + p->count) % kPipeSize;
            for (uint64_t i = 0; i < n; ++i) p->data[(tail + i) % kPipeSize] = in[done + i];
            p->count += uint32_t(n);
            done += n;
            wake(p->readable, p->waiting_readers);
            p->lock.unlock();
            notify();
            continue;
        }
        ++p->waiting_writers;
        p->lock.unlock();
        if (!p->writable.wait_interruptible()) return done ? int64_t(done) : kErrInterrupted;
    }
    return int64_t(done);
}

void pipe_close(void* context, uint32_t flags) {
    auto* p = static_cast<Pipe*>(context);
    p->lock.lock();
    if (flags & kRead) --p->readers;
    if (flags & kWrite) --p->writers;
    wake(p->readable, p->waiting_readers);  // readers see the end, writers see the broken pipe
    wake(p->writable, p->waiting_writers);
    p->lock.unlock();
    notify();
}

uint32_t pipe_poll(void* context, uint32_t) {
    auto* p = static_cast<Pipe*>(context);
    sync::ScopedLock guard(p->lock);
    uint32_t r = 0;
    if (p->count > 0 || p->writers == 0) r |= kPollIn;   // data, or the end of input
    if (p->count < kPipeSize) r |= kPollOut;
    if (p->writers == 0 || p->readers == 0) r |= kPollHup;
    return r;
}

void pipe_release(void* context) {
    auto* p = static_cast<Pipe*>(context);
    heap::free(p->data);
    heap::free(p);
}

const DeviceOps kPipeOps = {pipe_read, pipe_write, nullptr, pipe_close, pipe_release, nullptr, pipe_poll, nullptr, nullptr};

Node* make_dir_locked(Node* dir, const char* name) {
    Node* existing = child(dir, name, uint32_t(str::length(name)));
    if (existing) return existing->type == Type::Directory ? existing : nullptr;
    Node* n = nullptr;
    make(dir, name, Type::Directory, n);
    return n;
}

Node* make_device_locked(Node* dir, const char* name, const DeviceOps* ops, void* context) {
    Node* n = nullptr;
    if (make(dir, name, Type::Device, n) < 0) return nullptr;
    n->ops = ops;
    n->context = context;
    return n;
}

// ---------------------------------------------------------------- tar (ustar)

uint64_t octal(const uint8_t* s, uint32_t n) {
    uint64_t v = 0;
    for (uint32_t i = 0; i < n && s[i] >= '0' && s[i] <= '7'; ++i) v = v * 8 + (s[i] - '0');
    return v;
}

bool tar_header_ok(const uint8_t* h) {
    if (mem::compare(h + 257, "ustar", 5) != 0) return false;
    uint64_t sum = 0;
    for (uint32_t i = 0; i < 512; ++i) sum += (i >= 148 && i < 156) ? ' ' : h[i];
    return sum == octal(h + 148, 8);
}

bool all_zero(const uint8_t* p, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i)
        if (p[i]) return false;
    return true;
}

// Walks `rel` below `base`, creating missing directories, and returns the directory holding
// the last component (copied to `last`); with `last` null every component is a directory.
Node* make_dirs_locked(Node* base, const char* rel, char* last) {
    Node* dir = base;
    const char *p = rel, *s;
    uint32_t len;
    char name[kMaxName + 1];
    bool have = false;
    while (component(p, s, len)) {
        if (len > kMaxName) return nullptr;
        if (have) {
            dir = make_dir_locked(dir, name);
            if (!dir) return nullptr;
        }
        if (is_dot(s, len) || is_dotdot(s, len)) {  // archives we build never contain "..": just skip
            have = false;
            continue;
        }
        mem::copy(name, s, len);
        name[len] = 0;
        have = true;
    }
    if (!last) return have ? make_dir_locked(dir, name) : dir;
    if (!have) return nullptr;
    str::copy(last, name, kMaxName + 1);
    return dir;
}

}  // namespace

// ---------------------------------------------------------------- setup

void init() {
    Locked guard;
    if (g_root) return;
    g_root = new_node("", 0, Type::Directory);
    if (!g_root) panic("vfs: out of memory for the root directory");
    g_root->linked = true;
    g_kernel_cwd = g_root;
    ++g_root->refs;
    Node* dev = make_dir_locked(g_root, "dev");
    make_dir_locked(g_root, "bin");
    make_dir_locked(g_root, "etc");
    make_dir_locked(g_root, "tmp");
    make_dir_locked(g_root, "mnt");
    if (!dev) panic("vfs: out of memory for /dev");
    g_console = make_device_locked(dev, "console", &kConsoleOps, nullptr);
    make_device_locked(dev, "null", &kNullOps, nullptr);
    make_device_locked(dev, "zero", &kZeroOps, nullptr);
    if (!g_console) panic("vfs: out of memory for /dev/console");
}

int64_t unpack_tar(const char* directory, const void* data, uint64_t size) {
    auto* p = static_cast<const uint8_t*>(data);
    Node* base;
    {
        Locked guard;
        int64_t e = walk(directory, base);
        if (e < 0) return e;
        if (base->type != Type::Directory) return kErrNotDirectory;
    }
    int64_t entries = 0;
    for (uint64_t at = 0; at + 512 <= size;) {
        const uint8_t* h = p + at;
        if (all_zero(h, 512)) break;  // end of archive
        if (!tar_header_ok(h)) return entries ? entries : kErrBadArgument;
        uint64_t file_size = octal(h + 124, 12);
        uint64_t data_at = at + 512;
        if (data_at + file_size > size) return kErrBadArgument;
        at = data_at + mem::align_up(file_size, 512);

        char path[kMaxPath];  // prefix "/" name
        uint32_t n = 0;
        for (uint32_t i = 0; i < 155 && h[345 + i] && n < kMaxPath - 2; ++i) path[n++] = char(h[345 + i]);
        if (n) path[n++] = '/';
        for (uint32_t i = 0; i < 100 && h[i] && n < kMaxPath - 1; ++i) path[n++] = char(h[i]);
        path[n] = 0;

        uint8_t type = h[156];
        Locked guard;
        if (type == '5') {
            if (make_dirs_locked(base, path, nullptr)) ++entries;
        } else if (type == '0' || type == 0) {
            char name[kMaxName + 1];
            Node* dir = make_dirs_locked(base, path, name);
            if (!dir) continue;
            Node* f = child(dir, name, uint32_t(str::length(name)));
            if (f && f->type != Type::File) continue;
            if (!f && make(dir, name, Type::File, f) < 0) continue;
            drop_data(f);
            f->data = const_cast<uint8_t*>(p + data_at);
            f->size = f->capacity = file_size;
            f->borrowed = true;
            g_initrd_bytes += file_size;
            ++entries;
        }
        // links, devices and FIFOs are skipped
    }
    Locked guard;
    g_initrd_entries += uint64_t(entries);
    return entries;
}

void add_module(const char* path, const void* data, uint64_t size) {
    if (!path || !data) return;
    auto* bytes = static_cast<const uint8_t*>(data);
    if (size >= 512 && tar_header_ok(bytes)) {
        int64_t n = unpack_tar("/", data, size);
        term::write("[neko] initrd: ");
        if (n < 0) {
            term::write("\x1b[93mbroken archive\x1b[0m ");
            term::write(path);
            term::write("\n");
            return;
        }
        term::write_udec(uint64_t(n));
        term::write(" entries (");
        term::write_size(size);
        term::write(") from ");
        term::write(path);
        term::write(" unpacked into /\n");
        return;
    }
    // Any other module (an extra module_path line in limine.conf) becomes /bin/NAME.
    const char* base = path;
    for (const char* c = path; *c; ++c)
        if (*c == '/') base = c + 1;
    if (!*base || str::length(base) > kMaxName) return;
    Locked guard;
    Node* bin = make_dir_locked(g_root, "bin");
    if (!bin || child(bin, base, uint32_t(str::length(base)))) return;
    Node* f = nullptr;
    if (make(bin, base, Type::File, f) < 0) return;
    f->data = const_cast<uint8_t*>(bytes);
    f->size = f->capacity = size;
    f->borrowed = true;
    g_initrd_bytes += size;
}

// ---------------------------------------------------------------- files

int64_t open(const char* path, uint32_t flags, File*& out) {
    out = nullptr;
    if (!(flags & (kRead | kWrite))) return kErrBadArgument;
    if ((flags & kTruncate) && !(flags & kWrite)) return kErrBadArgument;
    Node* n = nullptr;  // with one extra reference while we work on it
    for (int attempt = 0;; ++attempt) {
        g_lock.lock();
        int64_t e = walk(path, n);
        if (e == 0) {
            if ((flags & kCreate) && (flags & kExclusive)) {
                g_lock.unlock();
                return kErrExists;
            }
            ++n->refs;
            g_lock.unlock();
            break;
        }
        if (e != kErrNotFound || !(flags & kCreate)) {
            g_lock.unlock();
            return e;
        }
        Node* dir;
        char name[kMaxName + 1];
        e = walk_parent(path, dir, name);
        if (e == 0 && !dir->fs_ops) {  // ramfs: create it right here
            e = make(dir, name, Type::File, n);
            if (e == 0) ++n->refs;
        }
        if (e < 0 || !dir->fs_ops) {
            g_lock.unlock();
            if (e < 0) return e;
            break;
        }
        ++dir->refs;  // a mounted volume: the driver writes the entry (I/O, so without the lock)
        g_lock.unlock();
        e = dir->fs_ops->create(dir->fs, dir, name, Type::File, n);
        node_release(dir);
        if (e == kErrExists && attempt < 3) continue;  // created meanwhile: open that one
        if (e < 0) return e;
        break;
    }
    int64_t e = 0;
    bool device_opened = false;
    if (n->type == Type::Device && n->ops->open) {  // may refuse (an exclusive device that is taken)
        e = n->ops->open(n->context, flags);
        device_opened = e == 0;
    }
    if (e < 0) {
    } else if (n->type == Type::Directory && (flags & kWrite)) e = kErrIsDirectory;
    else if (n->type == Type::File && (flags & kTruncate)) {
        if (n->fs_ops) e = n->fs_ops->truncate(n->fs, n, 0);
        else {
            Locked guard;
            e = resize(n, 0);
        }
    }
    if (e == 0) {
        Locked guard;
        out = new_file(n, flags);
        if (!out) e = kErrNoMemory;
    }
    if (e < 0 && device_opened && n->ops->close) n->ops->close(n->context, flags);
    node_release(n);
    return e;
}

int64_t read(File* f, void* buffer, uint64_t length) {
    if (!f) return kErrBadFd;
    if (!(f->flags & kRead)) return kErrAccess;
    Node* n = f->node;
    if (n->type == Type::Device) {
        if (!n->ops->read) return kErrNotSupported;
        if (!n->ops->size) return n->ops->read(n->context, 0, buffer, length);
        uint64_t at = __atomic_load_n(&f->offset, __ATOMIC_ACQUIRE);
        int64_t r = n->ops->read(n->context, at, buffer, length);
        if (r > 0) __atomic_store_n(&f->offset, at + uint64_t(r), __ATOMIC_RELEASE);
        return r;
    }
    if (n->type == Type::Directory) return kErrIsDirectory;
    if (n->fs_ops) {
        uint64_t at = __atomic_load_n(&f->offset, __ATOMIC_ACQUIRE);
        int64_t r = n->fs_ops->read(n->fs, n, at, buffer, length);
        if (r > 0) __atomic_store_n(&f->offset, at + uint64_t(r), __ATOMIC_RELEASE);
        return r;
    }
    Locked guard;
    if (f->offset >= n->size) return 0;
    uint64_t count = n->size - f->offset;
    if (count > length) count = length;
    mem::copy(buffer, n->data + f->offset, count);
    f->offset += count;
    return int64_t(count);
}

int64_t write(File* f, const void* buffer, uint64_t length) {
    if (!f) return kErrBadFd;
    if (!(f->flags & kWrite)) return kErrAccess;
    Node* n = f->node;
    if (n->type == Type::Device) {
        if (!n->ops->write) return kErrNotSupported;
        if (!n->ops->size) return n->ops->write(n->context, 0, buffer, length);
        uint64_t at = (f->flags & kAppend) ? n->ops->size(n->context) : __atomic_load_n(&f->offset, __ATOMIC_ACQUIRE);
        int64_t r = n->ops->write(n->context, at, buffer, length);
        if (r > 0) __atomic_store_n(&f->offset, at + uint64_t(r), __ATOMIC_RELEASE);
        return r;
    }
    if (n->type == Type::Directory) return kErrIsDirectory;
    if (length == 0) return 0;
    if (n->fs_ops) {
        uint64_t at = (f->flags & kAppend) ? node_size(n) : __atomic_load_n(&f->offset, __ATOMIC_ACQUIRE);
        if (at + length < at) return kErrBadArgument;
        int64_t r = n->fs_ops->write(n->fs, n, at, buffer, length);
        if (r > 0) __atomic_store_n(&f->offset, at + uint64_t(r), __ATOMIC_RELEASE);
        return r;
    }
    Locked guard;
    uint64_t at = (f->flags & kAppend) ? n->size : f->offset;
    if (at + length < at) return kErrBadArgument;
    uint64_t end = at + length;
    if (!reserve(n, end > n->size ? end : n->size)) return kErrNoMemory;
    if (at > n->size) mem::set(n->data + n->size, 0, at - n->size);  // a hole after seeking past the end
    mem::copy(n->data + at, buffer, length);
    if (end > n->size) n->size = end;
    n->modified_ms = time::uptime_ms();
    f->offset = end;
    return int64_t(length);
}

int64_t seek(File* f, int64_t offset, int32_t whence) {
    if (!f) return kErrBadFd;
    Node* n = f->node;
    bool device = n->type == Type::Device;
    if (device && !n->ops->size) return kErrNotSupported;
    uint64_t end = device ? n->ops->size(n->context) : 0;  // outside the lock: drivers may block
    Locked guard;
    int64_t base;
    switch (whence) {
        case kSeekSet: base = 0; break;
        case kSeekCurrent: base = int64_t(f->offset); break;
        case kSeekEnd: base = int64_t(device ? end : n->size); break;
        default: return kErrBadArgument;
    }
    int64_t to = base + offset;
    if (to < 0 || (offset > 0 && to < base)) return kErrBadArgument;
    f->offset = uint64_t(to);
    return to;
}

int64_t truncate(File* f, uint64_t size) {
    if (!f) return kErrBadFd;
    if (!(f->flags & kWrite)) return kErrAccess;
    if (f->node->type != Type::File) return f->node->type == Type::Directory ? kErrIsDirectory : kErrNotSupported;
    if (f->node->fs_ops) return f->node->fs_ops->truncate(f->node->fs, f->node, size);
    Locked guard;
    return resize(f->node, size);
}

int64_t fstat(File* f, Stat& out) {
    if (!f) return kErrBadFd;
    Locked guard;
    fill_stat(f->node, out);
    return 0;
}

int64_t readdir(File* f, uint32_t index, DirEntry& out) {
    if (!f) return kErrBadFd;
    Locked guard;
    if (f->node->type != Type::Directory) return kErrNotDirectory;
    Node* c = f->node->children;
    for (uint32_t i = 0; c && i < index; ++i) c = c->next;
    if (!c) return 0;
    mem::set(&out, 0, sizeof(out));
    str::copy(reinterpret_cast<char*>(out.name), c->name, sizeof(out.name));
    out.type = c->type;
    out.size = size_of(c);
    return 1;
}

File* dup(File* f) {
    if (f) __atomic_add_fetch(&f->refs, 1, __ATOMIC_ACQ_REL);
    return f;
}

void close(File* f) {
    if (!f || __atomic_sub_fetch(&f->refs, 1, __ATOMIC_ACQ_REL) > 0) return;
    Node* n = f->node;
    if (n->type == Type::Device && n->ops->close) n->ops->close(n->context, f->flags);
    {
        Locked guard;
        release(f->node);
        --g_open_files;
    }
    heap::free(f);
}

uint32_t flags(File* f) { return f ? f->flags : 0; }

namespace {

const DeviceOps kNoOps = {};

SocketIo g_socket_read = nullptr, g_socket_write = nullptr;
SocketClose g_socket_close = nullptr;

int64_t socket_read(void* context, uint64_t, void* buffer, uint64_t length) {
    return g_socket_read ? g_socket_read(context, static_cast<uint8_t*>(buffer), length) : kErrNotSupported;
}
int64_t socket_write(void* context, uint64_t, const void* buffer, uint64_t length) {
    return g_socket_write ? g_socket_write(context, static_cast<uint8_t*>(const_cast<void*>(buffer)), length) : kErrNotSupported;
}
void socket_close(void* context, uint32_t) {
    if (g_socket_close) g_socket_close(context);
}

const DeviceOps kSocketOps = {socket_read, socket_write, nullptr, socket_close, nullptr, nullptr, nullptr, nullptr, nullptr};

}  // namespace

void set_socket_callbacks(SocketIo read, SocketIo write, SocketClose close) {
    g_socket_read = read;
    g_socket_write = write;
    g_socket_close = close;
}

File* open_socket(void* context) {
    Locked guard;
    Node* n = new_node("socket", 6, Type::Device);
    if (!n) return nullptr;
    n->ops = &kSocketOps;
    n->context = context;
    File* f = new_file(n, kRead | kWrite);
    release(n);  // the open file keeps it (or, on failure, it goes now)
    return f;
}

void* socket_context(File* f) {
    if (!f || f->node->type != Type::Device || f->node->ops != &kSocketOps) return nullptr;
    return f->node->context;
}

File* open_object(const char* name, const DeviceOps* ops, void* context, uint32_t flags) {
    Locked guard;
    Node* n = new_node(name, uint32_t(str::length(name)), Type::Device);  // anonymous: never linked into the tree
    if (!n) return nullptr;
    n->ops = ops;
    n->context = context;
    File* f = new_file(n, flags);
    if (!f) n->ops = &kNoOps;  // the context stays the caller's: its release must not run
    release(n);                // the open file keeps the node
    return f;
}

void* object_context(File* f, const DeviceOps* ops) {
    if (!f || f->node->type != Type::Device || f->node->ops != ops) return nullptr;
    return f->node->context;
}

// ---------------------------------------------------------------- poll
//
// One wake-up for everybody: notify() bumps nothing per file, it just signals every poller, which looks
// at its files again. Cheap enough for a handful of waiting programs, and nothing can be missed: a poller
// is queued before it looks, so a notify() that comes after the look always reaches its semaphore.

namespace {

struct PollWaiter {
    sync::Semaphore wake;
    PollWaiter* next;
};
sync::SpinLock g_poll_lock;
PollWaiter* g_pollers = nullptr;

}  // namespace

uint32_t poll(File* f, uint32_t wanted) {
    if (!f) return 0;
    Node* n = f->node;
    uint32_t r;
    if (n->type == Type::Device && n->ops->poll) r = n->ops->poll(n->context, f->flags);
    else r = kPollIn | kPollOut;  // files and directories never block
    if (!(f->flags & kRead)) r &= ~kPollIn;
    if (!(f->flags & kWrite)) r &= ~kPollOut;
    return r & (wanted | kPollHup);
}

void notify() {
    sync::ScopedLock guard(g_poll_lock);
    for (PollWaiter* w = g_pollers; w; w = w->next) w->wake.signal();
}

int64_t poll_wait(File* const* files, const uint32_t* wanted, uint32_t* ready, uint32_t count, uint64_t timeout_ms) {
    PollWaiter w{};
    {
        sync::ScopedLock guard(g_poll_lock);
        w.next = g_pollers;
        g_pollers = &w;
    }
    uint64_t until = timeout_ms == ~0ull ? ~0ull : time::uptime_ms() + timeout_ms;
    int64_t result = 0;
    for (;;) {
        uint32_t n = 0;
        for (uint32_t i = 0; i < count; ++i) {
            ready[i] = poll(files[i], wanted[i]);
            if (ready[i]) ++n;
        }
        if (n || timeout_ms == 0) {
            result = n;
            break;
        }
        if (sched::killed()) {
            result = kErrInterrupted;
            break;
        }
        if (until == ~0ull) {
            if (!w.wake.wait_interruptible()) {
                result = kErrInterrupted;
                break;
            }
            continue;
        }
        // A deadline: semaphores cannot time out, so look every millisecond until a signal or the deadline.
        while (!w.wake.try_wait() && !sched::killed() && time::uptime_ms() < until) sched::sleep_ms(1);
        if (time::uptime_ms() >= until && !w.wake.try_wait()) {
            for (uint32_t i = 0; i < count; ++i) {  // one last look
                ready[i] = poll(files[i], wanted[i]);
                if (ready[i]) ++result;
            }
            break;
        }
    }
    sync::ScopedLock guard(g_poll_lock);
    for (PollWaiter** pp = &g_pollers; *pp; pp = &(*pp)->next) {
        if (*pp == &w) {
            *pp = w.next;
            break;
        }
    }
    return result;
}

int64_t map(File* f, uint64_t offset, uint64_t& phys, bool& write_combining) {
    write_combining = false;
    if (!f || f->node->type != Type::Device || !f->node->ops->map) return kErrNotSupported;
    return f->node->ops->map(f->node->context, offset, phys, write_combining);
}

int64_t control(File* f, uint64_t request, uint64_t arg) {
    if (!f || f->node->type != Type::Device || !f->node->ops->control) return kErrNotSupported;
    return f->node->ops->control(f->node->context, f->flags, request, arg);
}

int64_t pipe(File*& read_end, File*& write_end) {
    read_end = write_end = nullptr;
    auto* p = static_cast<Pipe*>(heap::alloc_zeroed(sizeof(Pipe)));
    uint8_t* data = static_cast<uint8_t*>(heap::alloc(kPipeSize));
    if (!p || !data) {
        heap::free(p);
        heap::free(data);
        return kErrNoMemory;
    }
    p->data = data;
    p->readers = p->writers = 1;
    Locked guard;
    Node* n = new_node("pipe", 4, Type::Device);  // anonymous: never linked into the tree
    if (!n) {
        pipe_release(p);
        return kErrNoMemory;
    }
    n->ops = &kPipeOps;
    n->context = p;
    read_end = new_file(n, kRead);
    write_end = new_file(n, kWrite);
    if (!read_end || !write_end) {  // undo without the close callbacks (nothing waits yet)
        if (read_end) { --n->refs; --g_open_files; heap::free(read_end); }
        if (write_end) { --n->refs; --g_open_files; heap::free(write_end); }
        read_end = write_end = nullptr;
        release(n);
        return kErrNoMemory;
    }
    release(n);  // the creation reference: the two open files keep it alive
    return 0;
}

// ---------------------------------------------------------------- names

int64_t stat(const char* path, Stat& out) {
    Locked guard;
    Node* n;
    int64_t e = walk(path, n);
    if (e < 0) return e;
    fill_stat(n, out);
    return 0;
}

int64_t mkdir(const char* path) {
    g_lock.lock();
    Node* dir;
    char name[kMaxName + 1];
    int64_t e = walk_parent(path, dir, name);
    if (e == 0 && child(dir, name, uint32_t(str::length(name)))) e = kErrExists;
    if (e < 0 || !dir->fs_ops) {
        Node* n;
        if (e == 0) e = make(dir, name, Type::Directory, n);
        g_lock.unlock();
        return e;
    }
    ++dir->refs;
    g_lock.unlock();
    Node* n = nullptr;
    e = dir->fs_ops->create(dir->fs, dir, name, Type::Directory, n);
    if (e == 0) node_release(n);
    node_release(dir);
    return e;
}

int64_t remove(const char* path) {
    g_lock.lock();
    Node* n;
    int64_t e = walk(path, n);
    if (e == 0 && (n == g_root || !n->parent || n->mount_root)) e = kErrBusy;
    if (e == 0 && n->type == Type::Directory && n->children) e = kErrNotEmpty;
    if (e < 0 || !n->fs_ops) {
        if (e == 0) {
            detach(n);
            release(n);
        }
        g_lock.unlock();
        return e;
    }
    // On a volume the clusters go at once, so an open file (or a working directory) keeps it busy.
    if (n->refs > 1) {
        g_lock.unlock();
        return kErrBusy;
    }
    ++n->refs;
    g_lock.unlock();
    e = n->fs_ops->remove(n->fs, n);
    node_release(n);
    return e;
}

int64_t rename(const char* from, const char* to) {
    g_lock.lock();
    Node* n;
    Node* dir = nullptr;
    char name[kMaxName + 1];
    Node* target = nullptr;
    int64_t e = walk(from, n);
    if (e == 0 && (n == g_root || !n->parent || n->mount_root)) e = kErrBusy;
    if (e == 0) e = walk_parent(to, dir, name);
    if (e == 0 && !dir->linked) e = kErrNotFound;
    if (e == 0) {
        for (Node* d = dir; d; d = d->parent)  // a directory cannot move into itself
            if (d == n) e = kErrBusy;
    }
    if (e == 0) target = child(dir, name, uint32_t(str::length(name)));
    if (e == 0 && target == n) {
        g_lock.unlock();
        return 0;
    }
    if (e == 0 && (n->fs_ops != dir->fs_ops || n->fs != dir->fs)) e = kErrNotSupported;  // across volumes: copy instead
    if (e == 0 && target) {
        if (target->type == Type::Directory && n->type != Type::Directory) e = kErrIsDirectory;
        else if (target->type != Type::Directory && n->type == Type::Directory) e = kErrNotDirectory;
        else if (target->type == Type::Directory && target->children) e = kErrNotEmpty;
        else if (target->mount_root) e = kErrBusy;
        else if (n->fs_ops && target->refs > 1) e = kErrBusy;
    }
    if (e < 0 || !n->fs_ops) {
        if (e == 0) {
            if (target) {
                detach(target);
                release(target);
            }
            detach(n);
            str::copy(n->name, name, sizeof(n->name));
            attach(dir, n);
        }
        g_lock.unlock();
        return e;
    }
    ++n->refs;
    ++dir->refs;
    if (target) ++target->refs;
    g_lock.unlock();
    if (target) {  // replacing a file: remove the old one first
        e = n->fs_ops->remove(n->fs, target);
        node_release(target);
    }
    if (e == 0) e = n->fs_ops->rename(n->fs, n, dir, name);
    node_release(dir);
    node_release(n);
    return e;
}

int64_t create_device(const char* path, const DeviceOps* ops, void* context) {
    if (!ops) return kErrBadArgument;
    Locked guard;
    Node* dir;
    char name[kMaxName + 1];
    int64_t e = walk_parent(path, dir, name);
    if (e < 0) return e;
    if (child(dir, name, uint32_t(str::length(name)))) return kErrExists;
    return make_device_locked(dir, name, ops, context) ? 0 : kErrNoMemory;
}

namespace {

constexpr uint64_t kGeneratedMax = 64 * 1024;

int64_t generated_read(void* context, uint64_t offset, void* buffer, uint64_t length) {
    auto generate = reinterpret_cast<Generator>(context);
    auto* text = static_cast<uint8_t*>(heap::alloc(kGeneratedMax));
    if (!text) return kErrNoMemory;
    int64_t n = generate(text, kGeneratedMax);
    int64_t r = 0;
    if (n < 0) r = n;
    else if (offset < uint64_t(n)) {
        r = int64_t(uint64_t(n) - offset < length ? uint64_t(n) - offset : length);
        mem::copy(buffer, text + offset, uint64_t(r));
    }
    heap::free(text);
    return r;
}

uint64_t generated_size(void* context) {
    auto generate = reinterpret_cast<Generator>(context);
    auto* text = static_cast<uint8_t*>(heap::alloc(kGeneratedMax));
    if (!text) return 0;
    int64_t n = generate(text, kGeneratedMax);
    heap::free(text);
    return n > 0 ? uint64_t(n) : 0;
}

const DeviceOps kGeneratedOps = {generated_read, nullptr, generated_size, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};

}  // namespace

int64_t create_generated(const char* path, Generator generate) {
    return create_device(path, &kGeneratedOps, reinterpret_cast<void*>(generate));
}

int64_t read_all(const char* path, uint8_t*& data, uint64_t max_size) {
    data = nullptr;
    File* f;
    int64_t e = open(path, kRead, f);
    if (e < 0) return e;
    Stat st;
    fstat(f, st);
    if (st.type != Type::File) {
        close(f);
        return st.type == Type::Directory ? kErrIsDirectory : kErrNotSupported;
    }
    if (st.size > max_size) {
        close(f);
        return kErrNoMemory;
    }
    auto* buffer = static_cast<uint8_t*>(heap::alloc(st.size ? st.size : 1));
    if (!buffer) {
        close(f);
        return kErrNoMemory;
    }
    int64_t got = 0;
    while (uint64_t(got) < st.size) {
        int64_t n = read(f, buffer + got, st.size - uint64_t(got));
        if (n <= 0) {
            if (n < 0) got = n;
            break;
        }
        got += n;
    }
    close(f);
    if (got < 0) {
        heap::free(buffer);
        return got;
    }
    data = buffer;
    return got;
}

// ---------------------------------------------------------------- working directory

int64_t chdir(const char* path) {
    Locked guard;
    Node* n;
    int64_t e = walk(path, n);
    if (e < 0) return e;
    if (n->type != Type::Directory) return kErrNotDirectory;
    ++n->refs;
    proc::Process* p = proc::current();
    Node*& slot = p && p->cwd ? p->cwd : g_kernel_cwd;
    Node* old = slot;
    slot = n;
    release(old);
    return 0;
}

int64_t getcwd(char* buffer, uint64_t capacity) {
    Locked guard;
    Node* n = cwd();
    if (!n->linked) return kErrNotFound;
    if (n == g_root) {
        if (capacity < 2) return kErrNameTooLong;
        buffer[0] = '/';
        buffer[1] = 0;
        return 1;
    }
    // Length first, then fill from the end.
    uint64_t length = 0;
    for (Node* d = n; d != g_root; d = d->parent) length += 1 + str::length(d->name);
    if (length + 1 > capacity) return kErrNameTooLong;
    buffer[length] = 0;
    uint64_t at = length;
    for (Node* d = n; d != g_root; d = d->parent) {
        uint64_t len = str::length(d->name);
        at -= len;
        mem::copy(buffer + at, d->name, len);
        buffer[--at] = '/';
    }
    return int64_t(length);
}

Node* cwd_retain() {
    Locked guard;
    Node* n = cwd();
    ++n->refs;
    return n;
}

void node_release(Node* n) {
    if (!n) return;
    Locked guard;
    release(n);
}

// ---------------------------------------------------------------- console

void set_console_reader(ConsoleReader reader) { __atomic_store_n(&g_console_reader, reader, __ATOMIC_RELEASE); }

File* open_console() {
    Locked guard;
    return g_console ? new_file(g_console, kRead | kWrite) : nullptr;
}

// ---------------------------------------------------------------- mounted volumes

int64_t mount(const char* path, const FsOps* ops, void* fs, const char* source, void* root_data, Node*& root) {
    Locked guard;
    Node* n;
    int64_t e = walk(path, n);
    if (e < 0) return e;
    if (n->type != Type::Directory) return kErrNotDirectory;
    if (n == g_root || n->fs_ops || n->children) return kErrBusy;  // an empty ramfs directory only
    Mount* m = nullptr;
    for (Mount& slot : g_mounts)
        if (!slot.root && !m) m = &slot;
    if (!m) return kErrLimit;
    m->root = n;
    m->ops = ops;
    m->fs = fs;
    str::copy(m->source, source ? source : "", sizeof(m->source));
    n->fs_ops = ops;
    n->fs = fs;
    n->fs_data = root_data;
    n->mount_root = true;
    ++n->refs;  // the mount's own reference
    root = n;
    return 0;
}

namespace {

bool busy_locked(Node* n, bool root) {
    if (n->refs > (root ? 2u : 1u)) return true;  // open files or working directories (root: + the mount's)
    for (Node* c = n->children; c; c = c->next)
        if (busy_locked(c, false)) return true;
    return false;
}

void drop_children_locked(Node* dir) {
    while (Node* c = dir->children) {
        drop_children_locked(c);
        detach(c);
        release(c);
    }
}

}  // namespace

int64_t unmount(const char* path) {
    Mount* m = nullptr;
    {
        Locked guard;
        Node* n;
        int64_t e = walk(path, n);
        if (e < 0) return e;
        for (Mount& slot : g_mounts)
            if (slot.root == n) m = &slot;
        if (!m) return kErrBadArgument;  // not a mount point
        if (busy_locked(n, true)) return kErrBusy;
    }
    m->ops->sync(m->fs);
    Node* root;
    const FsOps* ops;
    void* fs;
    {
        Locked guard;
        root = m->root;
        if (busy_locked(root, true)) return kErrBusy;  // opened while we synced
        ops = m->ops;
        fs = m->fs;
        drop_children_locked(root);
        if (root->fs_data) ops->release(fs, root->fs_data);
        root->fs_ops = nullptr;
        root->fs = nullptr;
        root->fs_data = nullptr;
        root->mount_root = false;
        m->root = nullptr;
        release(root);  // the mount's reference
    }
    ops->unmount(fs);
    return 0;
}

int64_t sync_all() {
    int64_t result = 0;
    for (Mount& m : g_mounts) {
        const FsOps* ops;
        void* fs;
        {
            Locked guard;
            if (!m.root) continue;
            ops = m.ops;
            fs = m.fs;
        }
        int64_t e = ops->sync(fs);
        if (e < 0) result = e;
    }
    return result;
}

bool mount_info(uint32_t index, MountInfo& out) {
    const FsOps* ops = nullptr;
    void* fs = nullptr;
    {
        Locked guard;
        uint32_t seen = 0;
        for (Mount& m : g_mounts) {
            if (!m.root) continue;
            if (seen++ != index) continue;
            mem::set(&out, 0, sizeof(out));
            // Path of the mount point, built from the end.
            char path[kMaxPath];
            uint64_t at = kMaxPath - 1;
            path[at] = 0;
            for (Node* d = m.root; d && d != g_root && at > 1; d = d->parent) {
                uint64_t len = str::length(d->name);
                if (len + 1 > at) break;
                at -= len;
                mem::copy(path + at, d->name, len);
                path[--at] = '/';
            }
            str::copy(reinterpret_cast<char*>(out.path), path + at, sizeof(out.path));
            str::copy(reinterpret_cast<char*>(out.source), m.source, sizeof(out.source));
            str::copy(reinterpret_cast<char*>(out.type), m.ops->type, sizeof(out.type));
            ops = m.ops;
            fs = m.fs;
            break;
        }
    }
    if (!ops) return false;
    ops->usage(fs, out.total_bytes, out.free_bytes);
    return true;
}

// ---- helpers for drivers

Node* add_node(Node* dir, const char* name, Type type, uint64_t size, void* fs_data, bool hold) {
    Locked guard;
    Node* n = new_node(name, uint32_t(str::length(name)), type);
    if (!n) return nullptr;
    n->size = size;
    n->fs_data = fs_data;
    attach(dir, n);
    if (hold) ++n->refs;
    return n;
}

void detach_node(Node* n) {
    Locked guard;
    if (!n->linked) return;
    detach(n);
    release(n);
}

void move_node(Node* n, Node* dir, const char* name) {
    Locked guard;
    detach(n);
    str::copy(n->name, name, sizeof(n->name));
    attach(dir, n);
}

void set_size(Node* n, uint64_t size) {
    Locked guard;
    n->size = size;
    n->modified_ms = time::uptime_ms();
}

uint64_t node_size(Node* n) {
    Locked guard;
    return n->size;
}

void* node_data(Node* n) { return n->fs_data; }

bool has_children(Node* n) {
    Locked guard;
    return n->children != nullptr;
}

bool has_child(Node* dir, const char* name) {
    Locked guard;
    return child(dir, name, uint32_t(str::length(name))) != nullptr;
}

Stats stats() {
    Locked guard;
    return Stats{g_nodes, g_open_files, g_heap_bytes, g_initrd_bytes, g_initrd_entries};
}

}  // namespace neko::vfs
