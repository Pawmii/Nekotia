// User processes: the ELF loader, the process table, exit/wait, file descriptors and the
// system call table (see neko/process.hpp for the ABI and the address-space layout).
//
// Life of a process (one or more threads sharing an address space):
//   spawn  — read the program from the VFS, check the ELF, build a fresh address space (image +
//            stack with argc/argv), inherit fds 0–2 and the working directory, start a kernel
//            thread bound to it (sched::spawn_for)
//   start  — that thread loads a clean FPU state and drops to ring 3 (arch::enter_user)
//   run    — system calls arrive through neko_syscall_dispatch; CPU exceptions in ring 3 end the
//            process (user_exception) instead of stopping the kernel
//   exit   — exit(), kill, Ctrl+C or a fault fix the exit code and mark every thread kill_pending
//            (sched::interrupt also cuts short sleeps and interruptible waits); each thread ends
//            on its way back to ring 3 (check_killed). Once the last one is off its CPU,
//            thread_freed frees the address space, closes the files and wakes the parent in wait()
//   wait   — the parent collects the exit code and the Process record is freed
#include <neko/process.hpp>
#include <neko/runtime.hpp>
#include <neko/vfs.hpp>

extern "C" uint64_t neko_user_copy(void* dst, const void* src, uint64_t length);  // syscall.S: 0 or -1 on #PF

namespace neko::proc {
namespace {

// ---------------------------------------------------------------- process table

sync::SpinLock g_lock;
Process* g_processes = nullptr;
uint32_t g_next_pid = 1;
uint32_t g_count = 0;

SyscallHandler g_syscalls[kMaxSyscalls];
bool g_report_faults = true;

void copy_name(uint8_t* dst, const char* src) {
    uint32_t i = 0;
    for (; src && src[i] && i < 23; ++i) dst[i] = uint8_t(src[i]);
    dst[i] = 0;
}

Process* find_locked(uint32_t pid) {
    for (Process* p = g_processes; p; p = p->next)
        if (p->pid == pid) return p;
    return nullptr;
}

void unlink_locked(Process* p) {
    for (Process** pp = &g_processes; *pp; pp = &(*pp)->next) {
        if (*pp == p) {
            *pp = p->next;
            --g_count;
            return;
        }
    }
}

// ---------------------------------------------------------------- ELF64

struct ElfHeader {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct ProgramHeader {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

constexpr uint32_t kPtLoad = 1;
constexpr uint32_t kPfExecute = 1, kPfWrite = 2;
constexpr uint64_t kPage = pmm::kPageSize;

bool add_overflows(uint64_t a, uint64_t b) { return a + b < a; }

// Checks everything the loader relies on, so loading itself cannot fail on a bad file.
bool valid_elf(const uint8_t* data, uint64_t size) {
    if (size < sizeof(ElfHeader)) return false;
    auto* h = reinterpret_cast<const ElfHeader*>(data);
    if (h->ident[0] != 0x7F || h->ident[1] != 'E' || h->ident[2] != 'L' || h->ident[3] != 'F') return false;
    if (h->ident[4] != 2 || h->ident[5] != 1) return false;  // 64-bit, little endian
    if (h->type != 2 || h->machine != 62) return false;      // ET_EXEC, x86_64 (static, not PIE)
    if (h->phentsize != sizeof(ProgramHeader) || h->phnum == 0 || h->phnum > 64) return false;
    if (h->phoff > size || h->phoff + uint64_t(h->phnum) * sizeof(ProgramHeader) > size) return false;

    bool entry_ok = false;
    auto* ph = reinterpret_cast<const ProgramHeader*>(data + h->phoff);
    for (uint16_t i = 0; i < h->phnum; ++i) {
        const ProgramHeader& p = ph[i];
        if (p.type != kPtLoad) continue;
        if (p.filesz > p.memsz || add_overflows(p.offset, p.filesz) || p.offset + p.filesz > size) return false;
        if (p.vaddr < kUserImageBase || add_overflows(p.vaddr, p.memsz) || p.vaddr + p.memsz > kUserAllocBase) return false;
        if ((p.flags & kPfExecute) && h->entry >= p.vaddr && h->entry < p.vaddr + p.memsz) entry_ok = true;
    }
    return entry_ok;
}

// Writes `length` bytes at user address `virt` of a space that is not active (through the HHDM).
void poke(uint64_t* pml4, uint64_t virt, const void* src, uint64_t length) {
    auto* from = static_cast<const uint8_t*>(src);
    while (length) {
        uint64_t phys = vmm::space_translate(pml4, virt);
        uint64_t chunk = kPage - (virt & (kPage - 1));
        if (chunk > length) chunk = length;
        if (phys != ~0ull) mem::copy(pmm::phys_to_virt(phys), from, chunk);
        virt += chunk;
        from += chunk;
        length -= chunk;
    }
}

// Maps fresh zeroed pages for [start, end) (page-aligned); widens pages that are already there.
bool map_pages(Process* p, uint64_t start, uint64_t end, vmm::PageFlags flags) {
    sync::ScopedLock guard(p->lock);
    for (uint64_t v = start; v < end; v += kPage) {
        if (vmm::space_translate(p->pml4, v) != ~0ull) {
            vmm::space_add_flags(p->pml4, v, flags);
            continue;
        }
        void* frame = pmm::alloc_page();
        if (!frame) return false;
        if (!vmm::space_map(p->pml4, v, pmm::virt_to_phys(frame), flags)) {
            pmm::free_page(frame);
            return false;
        }
        ++p->user_pages;
    }
    return true;
}

// Where a new thread enters ring 3 (heap-allocated, freed by the thread before it gets there).
struct StartInfo {
    Process* process;
    uint64_t rip, rsp, arg0, arg1;
};

bool load_image(Process* p, const uint8_t* data, StartInfo& start) {
    auto* h = reinterpret_cast<const ElfHeader*>(data);
    auto* ph = reinterpret_cast<const ProgramHeader*>(data + h->phoff);
    for (uint16_t i = 0; i < h->phnum; ++i) {
        const ProgramHeader& s = ph[i];
        if (s.type != kPtLoad || s.memsz == 0) continue;
        vmm::PageFlags flags = vmm::PageFlags::None;
        if (s.flags & kPfWrite) flags = flags | vmm::PageFlags::Write;
        if (s.flags & kPfExecute) flags = flags | vmm::PageFlags::Execute;
        uint64_t start = mem::align_down(s.vaddr, kPage);
        uint64_t end = mem::align_up(s.vaddr + s.memsz, kPage);
        if (!map_pages(p, start, end, flags)) return false;
        poke(p->pml4, s.vaddr, data + s.offset, s.filesz);  // the rest (.bss) stays zero
    }
    start.rip = h->entry;
    return true;
}

// Stack: [argv[0] … argv[n-1] strings][padding][argv pointers, NULL] ← rsp (16-byte aligned).
bool build_stack(Process* p, const char* name, const char* args, StartInfo& start) {
    constexpr uint32_t kMaxArgs = 32;
    constexpr uint64_t kMaxArgBytes = 2048;
    if (!map_pages(p, kUserStackTop - kUserStackSize, kUserStackTop, vmm::PageFlags::Write)) return false;

    char strings[kMaxArgBytes];
    uint64_t offsets[kMaxArgs];
    uint32_t argc = 0;
    uint64_t used = 0;
    auto add = [&](const char* s, uint64_t n) {
        if (argc == kMaxArgs || used + n + 1 > kMaxArgBytes) return false;
        offsets[argc++] = used;
        mem::copy(strings + used, s, n);
        strings[used + n] = 0;
        used += n + 1;
        return true;
    };
    if (!add(name, str::length(name))) return false;
    for (const char* a = args; a && *a;) {
        while (*a == ' ') ++a;
        if (!*a) break;
        const char* b = a;
        while (*b && *b != ' ') ++b;
        if (!add(a, uint64_t(b - a))) return false;
        a = b;
    }

    uint64_t strings_at = mem::align_down(kUserStackTop - used, 16);
    poke(p->pml4, strings_at, strings, used);
    uint64_t pointers[kMaxArgs + 1];
    for (uint32_t i = 0; i < argc; ++i) pointers[i] = strings_at + offsets[i];
    pointers[argc] = 0;
    uint64_t argv_at = mem::align_down(strings_at - (argc + 1) * 8, 16);
    poke(p->pml4, argv_at, pointers, (argc + 1) * 8);

    start.arg0 = argc;
    start.arg1 = argv_at;
    start.rsp = argv_at;
    p->alloc_next = kUserAllocBase;
    return true;
}

// ---------------------------------------------------------------- process thread

alignas(16) uint8_t g_clean_fpu[512];
uint32_t g_foreground = 0;  // pid owning the console (0 = kernel shell)

// First code of every user thread (kernel mode, already on the process's page tables).
void thread_main(uint64_t arg) {
    auto* s = reinterpret_cast<StartInfo*>(arg);
    StartInfo start = *s;
    heap::free(s);
    check_killed();  // killed before it ever ran
    // Nothing of the kernel's FPU/SSE state may leak into the program.
    __asm__ volatile("fxrstor %0" : : "m"(g_clean_fpu));
    arch::enter_user(start.rip, start.rsp, start.arg0, start.arg1);
}

void init_clean_fpu() {
    static bool ready = false;
    if (ready) return;
    mem::set(g_clean_fpu, 0, sizeof(g_clean_fpu));
    g_clean_fpu[0] = 0x7F;  // FCW 0x037F: x87 exceptions masked, extended precision
    g_clean_fpu[1] = 0x03;
    reinterpret_cast<uint32_t*>(g_clean_fpu)[6] = 0x1F80;  // MXCSR: SSE exceptions masked
    ready = true;
}

// Starts one more thread of `p` (the counter p->threads is already incremented by the caller).
uint32_t start_thread(Process* p, const StartInfo& start) {
    auto* s = static_cast<StartInfo*>(heap::alloc(sizeof(StartInfo)));
    if (!s) return 0;
    *s = start;
    uint32_t tid = sched::spawn_for(p, reinterpret_cast<const char*>(p->name), &thread_main, reinterpret_cast<uint64_t>(s));
    if (!tid) heap::free(s);
    return tid;
}

// Closes every file descriptor and drops the working directory.
void close_files(Process* p) {
    for (uint32_t i = 0; i < kMaxFiles; ++i) {
        vfs::File* f;
        {
            sync::ScopedLock guard(p->lock);
            f = p->files[i];
            p->files[i] = nullptr;
        }
        vfs::close(f);
    }
    vfs::node_release(p->cwd);
    p->cwd = nullptr;
}

void release(Process* p) {
    close_files(p);
    vmm::space_destroy(p->pml4);
    heap::free(p);
}

// fds 0–2 of a new process: the parent's, or the console for programs the kernel starts.
bool inherit_files(Process* p, Process* parent) {
    if (parent) {
        sync::ScopedLock guard(parent->lock);
        for (uint32_t i = 0; i < 3; ++i) p->files[i] = vfs::dup(parent->files[i]);
    } else {
        vfs::File* console = vfs::open_console();
        if (!console) return false;
        p->files[0] = console;
        p->files[1] = vfs::dup(console);
        p->files[2] = vfs::dup(console);
    }
    p->cwd = vfs::cwd_retain();
    return true;
}

const char* base_name(const char* path) {
    const char* base = path;
    for (const char* c = path; *c; ++c)
        if (*c == '/') base = c + 1;
    return base;
}

constexpr uint64_t kMaxProgramSize = 64ull << 20;

// g_lock held. Fixes the exit code (first one wins) and ends every thread of p except the caller.
void terminate_locked(Process* p, int64_t code) {
    if (p->state == State::Exited) return;
    if (!p->exiting) {
        p->exiting = true;
        p->exit_code = code;
    }
    // Collect first (the scheduler lock is held inside for_each_thread), then interrupt.
    // The threads cannot be freed meanwhile: thread_freed needs g_lock, which we hold.
    sched::Thread* list[kMaxThreads + 1];
    struct Ctx {
        sched::Thread** list;
        uint32_t n;
        sched::Thread* self;
    } ctx{list, 0, sched::current()};
    sched::for_each_thread(p, [](sched::Thread* t, void* c) {
        auto* x = static_cast<Ctx*>(c);
        if (t != x->self && x->n < kMaxThreads + 1) x->list[x->n++] = t;
    }, &ctx);
    for (uint32_t i = 0; i < ctx.n; ++i) sched::interrupt(list[i]);
}

}  // namespace

// ---------------------------------------------------------------- processes

Process* current() {
    sched::Thread* t = sched::current();
    return t ? t->process : nullptr;
}

int64_t spawn(const char* name, const char* args, bool detached) {
    if (!name || !*name) return kErrBadArgument;
    if (!arch::user_mode_available()) return kErrNoSyscall;
    // A bare name is a program in /bin; anything with a slash is a path.
    char path[vfs::kMaxPath];
    bool has_slash = false;
    for (const char* c = name; *c; ++c) has_slash |= *c == '/';
    if (has_slash) {
        str::copy(path, name, sizeof(path));
    } else {
        if (str::length(name) > vfs::kMaxName) return kErrNotFound;
        str::copy(path, "/bin/", sizeof(path));
        str::copy(path + 5, name, sizeof(path) - 5);
    }
    uint8_t* image = nullptr;
    int64_t size = vfs::read_all(path, image, kMaxProgramSize);
    if (size == kErrIsDirectory || size == kErrNotSupported) return kErrBadExecutable;
    if (size < 0) return size;
    const char* program = base_name(path);
    if (!valid_elf(image, uint64_t(size))) {
        heap::free(image);
        return kErrBadExecutable;
    }

    auto* p = static_cast<Process*>(heap::alloc_zeroed(sizeof(Process)));
    if (!p) {
        heap::free(image);
        return kErrNoMemory;
    }
    Process* parent = current();
    p->pml4 = vmm::space_create();
    if (!p->pml4 || !inherit_files(p, parent)) {
        heap::free(image);
        release(p);
        return kErrNoMemory;
    }
    p->pml4_phys = pmm::virt_to_phys(p->pml4);
    StartInfo start{p, 0, 0, 0, 0};
    bool loaded = load_image(p, image, start) && build_stack(p, program, args, start);
    heap::free(image);
    if (!loaded) {
        release(p);
        return kErrNoMemory;
    }
    p->alloc_next = kUserAllocBase;
    copy_name(p->name, program);
    p->parent = parent ? parent->pid : 0;
    p->detached = detached;
    p->started_ms = time::uptime_ms();
    p->threads = 1;
    init_clean_fpu();

    {
        sync::ScopedLock guard(g_lock);
        p->pid = g_next_pid++;
        p->next = g_processes;
        g_processes = p;
        ++g_count;
    }
    uint32_t pid = p->pid;
    uint32_t tid = start_thread(p, start);
    if (!tid) {
        {
            sync::ScopedLock guard(g_lock);
            unlink_locked(p);
        }
        release(p);
        return kErrNoMemory;
    }
    p->thread_id = tid;  // informational; the thread may already be running (or gone, if detached)
    return pid;
}

int64_t wait(uint32_t pid) {
    Process* me = current();
    uint32_t caller = me ? me->pid : 0;
    Process* p;
    bool took_console = false;
    {
        sync::ScopedLock guard(g_lock);
        p = find_locked(pid);
        if (!p) return kErrNotFound;
        if (p->parent != caller || p->detached || p->waited) return kErrNotChild;
        p->waited = true;
        if (g_foreground == caller && p->state == State::Running) {  // the console goes to the child
            g_foreground = pid;
            took_console = true;
        }
    }
    bool done = me ? p->exited.wait_interruptible() : (p->exited.wait(), true);
    Process* to_free = nullptr;
    int64_t code = kErrInterrupted;
    {
        sync::ScopedLock guard(g_lock);
        if (took_console && g_foreground == pid) g_foreground = caller;
        if (done) {
            unlink_locked(p);
            code = p->exit_code;
            to_free = p;
        } else if (p->state == State::Exited) {  // we are being killed; the child is gone too
            unlink_locked(p);
            to_free = p;
        } else {  // we are being killed: nobody will collect the child now
            p->waited = false;
            p->detached = true;
            p->parent = 0;
        }
    }
    if (to_free) heap::free(to_free);
    return code;
}

void check_killed() {
    if (sched::killed()) sched::exit();
}

void exit_current(int64_t code) {
    Process* p = current();
    if (!p) panic("proc::exit_current called from a kernel thread");
    {
        sync::ScopedLock guard(g_lock);
        terminate_locked(p, code);
    }
    sched::exit();
}

int64_t kill(uint32_t pid, int64_t code) {
    sync::ScopedLock guard(g_lock);
    Process* p = find_locked(pid);
    if (!p || p->state == State::Exited) return kErrNotFound;
    terminate_locked(p, code);
    if (p == current()) __atomic_store_n(&sched::current()->kill_pending, true, __ATOMIC_SEQ_CST);
    return 0;
}

bool interrupt_foreground() {
    uint32_t fg = __atomic_load_n(&g_foreground, __ATOMIC_ACQUIRE);
    return fg != 0 && kill(fg, kExitInterrupted) == 0;
}

uint32_t foreground() { return g_foreground; }

void thread_freed(sched::Thread* t) {
    Process* p = t->process;
    {
        sync::ScopedLock guard(g_lock);
        if (--p->threads > 0) return;  // other threads still run
    }
    vmm::space_destroy(p->pml4);
    p->pml4 = nullptr;
    close_files(p);
    bool free_now;
    {
        sync::ScopedLock guard(g_lock);
        p->state = State::Exited;
        if (!p->exiting) {  // the last thread ended without exit(): code 0
            p->exiting = true;
            p->exit_code = 0;
        }
        p->user_pages = 0;
        free_now = p->detached;
        if (free_now) unlink_locked(p);
        // Children nobody will wait for any more: running ones are freed when they exit,
        // finished ones right now (below).
        for (Process* q = g_processes; q; q = q->next) {
            if (q->parent != p->pid || q->waited) continue;
            q->parent = 0;
            q->detached = true;
        }
    }
    for (;;) {  // reap orphans that had already exited
        Process* z = nullptr;
        {
            sync::ScopedLock guard(g_lock);
            for (Process* q = g_processes; q; q = q->next)
                if (q->detached && q->state == State::Exited) z = q;
            if (z) unlink_locked(z);
        }
        if (!z) break;
        heap::free(z);
    }
    if (free_now) heap::free(p);
    else p->exited.signal();
}

uint32_t count() { return g_count; }

bool info(uint32_t index, ProcessInfo& out) {
    sync::ScopedLock guard(g_lock);
    Process* p = g_processes;
    for (uint32_t i = 0; p && i < index; ++i) p = p->next;
    if (!p) return false;
    out.pid = p->pid;
    out.parent = p->parent;
    out.thread_id = p->thread_id;
    out.state = p->state;
    mem::copy(out.name, p->name, sizeof(out.name));
    out.memory_kib = p->user_pages * (kPage / 1024);
    out.uptime_ms = time::uptime_ms() - p->started_ms;
    out.threads = p->threads;
    return true;
}

// ---------------------------------------------------------------- CPU exceptions in ring 3

void set_fault_reports(bool on) { g_report_faults = on; }

void user_exception(const crash::CpuState* f) {
    Process* p = current();
    if (!p) panic("user-mode exception without a process");
    int64_t code = kKilledBase + int64_t(f->vector);
    if (!g_report_faults) exit_current(code);
    const char* what;
    switch (f->vector) {
        case 0: what = "System.DivideByZeroException"; break;
        case 3: what = "System.Diagnostics.Debugger.Break (int3)"; break;
        case 6: what = "System.InvalidProgramException (invalid instruction)"; break;
        case 13: what = "System.AccessViolationException (general protection: privileged instruction or bad address)"; break;
        case 14: what = cpu::read_cr2() < 0x10000 ? "System.NullReferenceException" : "System.AccessViolationException"; break;
        case 16:
        case 19: what = "System.ArithmeticException (floating point)"; break;
        default: what = "CPU exception"; break;
    }
    term::write("\x1b[91m[neko] ");
    term::write(reinterpret_cast<const char*>(p->name));
    term::write(" (pid ");
    term::write_udec(p->pid);
    term::write(", thread ");
    term::write_udec(sched::current_id());
    term::write("): ");
    term::write(what);
    term::write("\x1b[0m\n       ");
    if (f->vector == 14) {
        bool present = f->error_code & 1, write = f->error_code & 2, fetch = f->error_code & 16;
        term::write(fetch ? "instruction fetch from" : write ? "write to" : "read from");
        term::write(present ? " a protected page at " : " an unmapped page at ");
        term::write_hex(cpu::read_cr2());
        term::write(", ");
    }
    term::write("rip ");
    term::write_hex(f->rip);
    term::write(" — process terminated, exit code ");
    term::write_udec(uint64_t(code));
    term::write("\n");
    exit_current(code);
}

// ---------------------------------------------------------------- user memory

bool register_syscall(uint32_t number, SyscallHandler handler) {
    if (number < kFirstFreeSyscall) return false;  // runtime-owned
    if (number >= kMaxSyscalls || !handler || g_syscalls[number]) return false;
    g_syscalls[number] = handler;
    return true;
}

bool check_read(const void* address, uint64_t length) {
    Process* p = current();
    return p && vmm::space_check(p->pml4, reinterpret_cast<uint64_t>(address), length, false);
}

bool check_write(const void* address, uint64_t length) {
    Process* p = current();
    return p && vmm::space_check(p->pml4, reinterpret_cast<uint64_t>(address), length, true);
}

namespace {
bool user_range(const void* address, uint64_t length) {
    auto a = reinterpret_cast<uint64_t>(address);
    return current() && a + length >= a && a + length <= kUserEnd;
}
}  // namespace

bool copy_from_user(void* dst, const void* user_src, uint64_t length) {
    if (length == 0) return true;
    return user_range(user_src, length) && neko_user_copy(dst, user_src, length) == 0;
}

bool copy_to_user(void* user_dst, const void* src, uint64_t length) {
    if (length == 0) return true;
    return user_range(user_dst, length) && neko_user_copy(user_dst, src, length) == 0;
}

int64_t copy_string(const void* user, char* out, uint64_t capacity) {
    auto* s = static_cast<const char*>(user);
    for (uint64_t i = 0; i < capacity; ++i) {
        if (!copy_from_user(&out[i], s + i, 1)) return kErrBadAddress;
        if (!out[i]) return int64_t(i);
    }
    return kErrBadArgument;  // too long
}

// ---------------------------------------------------------------- system calls

namespace {

// ---- file descriptors

// The open file behind `fd` with one extra reference (drop it with vfs::close), or nullptr.
vfs::File* take(uint64_t fd) {
    Process* p = current();
    if (fd >= kMaxFiles) return nullptr;
    sync::ScopedLock guard(p->lock);
    return vfs::dup(p->files[fd]);
}

// Puts `f` in the lowest free slot at or above `from`; the fd, or kErrTooManyFiles (f is closed then).
int64_t install(vfs::File* f, uint32_t from = 0) {
    Process* p = current();
    {
        sync::ScopedLock guard(p->lock);
        for (uint32_t i = from; i < kMaxFiles; ++i) {
            if (!p->files[i]) {
                p->files[i] = f;
                return i;
            }
        }
    }
    vfs::close(f);
    return kErrTooManyFiles;
}

constexpr uint64_t kChunk = 16 * 1024;  // bounce buffer between user memory and the VFS

int64_t sys_read(uint64_t fd, uint64_t buffer, uint64_t length) {
    vfs::File* f = take(fd);
    if (!f) return kErrBadFd;
    auto* dst = reinterpret_cast<uint8_t*>(buffer);
    if (length > (1u << 20)) length = 1u << 20;
    int64_t r = 0;
    if (length && !check_write(dst, length)) r = kErrBadAddress;
    uint8_t* bounce = nullptr;
    if (r == 0 && length) {
        bounce = static_cast<uint8_t*>(heap::alloc(length < kChunk ? length : kChunk));
        if (!bounce) r = kErrNoMemory;
    }
    // Files fill the whole buffer chunk by chunk; a device answers once (the console: one line).
    vfs::Stat st;
    bool regular = vfs::fstat(f, st) == 0 && st.type == vfs::Type::File;
    uint64_t done = 0;
    while (r == 0 && done < length) {
        uint64_t n = length - done < kChunk ? length - done : kChunk;
        int64_t got = vfs::read(f, bounce, n);
        if (got < 0) {
            if (done == 0) r = got;
            break;
        }
        if (got && !copy_to_user(dst + done, bounce, uint64_t(got))) {
            if (done == 0) r = kErrBadAddress;
            break;
        }
        done += uint64_t(got);
        if (!regular || uint64_t(got) < n || sched::killed()) break;
    }
    heap::free(bounce);
    vfs::close(f);
    return r < 0 ? r : int64_t(done);
}

int64_t sys_write(uint64_t fd, uint64_t buffer, uint64_t length) {
    vfs::File* f = take(fd);
    if (!f) return kErrBadFd;
    auto* src = reinterpret_cast<const uint8_t*>(buffer);
    if (length > (16u << 20)) length = 16u << 20;
    int64_t r = 0;
    if (length && !check_read(src, length)) r = kErrBadAddress;
    uint8_t* bounce = nullptr;
    if (r == 0 && length) {
        bounce = static_cast<uint8_t*>(heap::alloc(length < kChunk ? length : kChunk));
        if (!bounce) r = kErrNoMemory;
    }
    uint64_t done = 0;
    while (r == 0 && done < length) {
        uint64_t n = length - done < kChunk ? length - done : kChunk;
        if (!copy_from_user(bounce, src + done, n)) {
            if (done == 0) r = kErrBadAddress;
            break;
        }
        int64_t put = vfs::write(f, bounce, n);
        if (put < 0) {
            if (done == 0) r = put;
            break;
        }
        done += uint64_t(put);
        if (uint64_t(put) < n) break;
    }
    heap::free(bounce);
    vfs::close(f);
    return r < 0 ? r : int64_t(done);
}

// A path argument: copied into `out` (vfs::kMaxPath bytes).
int64_t user_path(uint64_t address, char* out) {
    int64_t n = copy_string(reinterpret_cast<const void*>(address), out, vfs::kMaxPath);
    if (n == kErrBadArgument) return kErrNameTooLong;
    return n < 0 ? n : 0;
}

int64_t sys_open(uint64_t path, uint64_t flags) {
    char p[vfs::kMaxPath];
    if (int64_t e = user_path(path, p)) return e;
    vfs::File* f;
    int64_t e = vfs::open(p, uint32_t(flags), f);
    if (e < 0) return e;
    return install(f);
}

int64_t sys_close(uint64_t fd) {
    Process* p = current();
    if (fd >= kMaxFiles) return kErrBadFd;
    vfs::File* f;
    {
        sync::ScopedLock guard(p->lock);
        f = p->files[fd];
        p->files[fd] = nullptr;
    }
    if (!f) return kErrBadFd;
    vfs::close(f);
    return 0;
}

int64_t sys_seek(uint64_t fd, uint64_t offset, uint64_t whence) {
    vfs::File* f = take(fd);
    if (!f) return kErrBadFd;
    int64_t r = vfs::seek(f, int64_t(offset), int32_t(whence));
    vfs::close(f);
    return r;
}

int64_t sys_stat(uint64_t path, uint64_t out) {
    char p[vfs::kMaxPath];
    if (int64_t e = user_path(path, p)) return e;
    vfs::Stat st;
    int64_t e = vfs::stat(p, st);
    if (e < 0) return e;
    return copy_to_user(reinterpret_cast<void*>(out), &st, sizeof(st)) ? 0 : kErrBadAddress;
}

int64_t sys_fstat(uint64_t fd, uint64_t out) {
    vfs::File* f = take(fd);
    if (!f) return kErrBadFd;
    vfs::Stat st;
    int64_t e = vfs::fstat(f, st);
    vfs::close(f);
    if (e < 0) return e;
    return copy_to_user(reinterpret_cast<void*>(out), &st, sizeof(st)) ? 0 : kErrBadAddress;
}

int64_t sys_readdir(uint64_t fd, uint64_t index, uint64_t out) {
    vfs::File* f = take(fd);
    if (!f) return kErrBadFd;
    vfs::DirEntry entry;
    int64_t r = index > 0xFFFFFFFFu ? 0 : vfs::readdir(f, uint32_t(index), entry);
    vfs::close(f);
    if (r == 1 && !copy_to_user(reinterpret_cast<void*>(out), &entry, sizeof(entry))) return kErrBadAddress;
    return r;
}

int64_t sys_path_op(uint64_t path, int64_t (*op)(const char*)) {
    char p[vfs::kMaxPath];
    if (int64_t e = user_path(path, p)) return e;
    return op(p);
}

int64_t sys_rename(uint64_t from, uint64_t to) {
    char a[vfs::kMaxPath], b[vfs::kMaxPath];
    if (int64_t e = user_path(from, a)) return e;
    if (int64_t e = user_path(to, b)) return e;
    return vfs::rename(a, b);
}

int64_t sys_dup(uint64_t fd) {
    vfs::File* f = take(fd);
    return f ? install(f) : kErrBadFd;
}

int64_t sys_dup2(uint64_t fd, uint64_t target) {
    if (target >= kMaxFiles) return kErrBadFd;
    vfs::File* f = take(fd);
    if (!f) return kErrBadFd;
    if (fd == target) {
        vfs::close(f);
        return int64_t(target);
    }
    Process* p = current();
    vfs::File* old;
    {
        sync::ScopedLock guard(p->lock);
        old = p->files[target];
        p->files[target] = f;
    }
    vfs::close(old);
    return int64_t(target);
}

int64_t sys_getcwd(uint64_t buffer, uint64_t capacity) {
    char path[vfs::kMaxPath];
    int64_t n = vfs::getcwd(path, sizeof(path));
    if (n < 0) return n;
    if (uint64_t(n) + 1 > capacity) return kErrNameTooLong;
    return copy_to_user(reinterpret_cast<void*>(buffer), path, uint64_t(n) + 1) ? n : kErrBadAddress;
}

int64_t sys_truncate(uint64_t fd, uint64_t size) {
    vfs::File* f = take(fd);
    if (!f) return kErrBadFd;
    int64_t r = vfs::truncate(f, size);
    vfs::close(f);
    return r;
}

int64_t sys_pipe(uint64_t out) {
    if (!check_write(reinterpret_cast<void*>(out), 8)) return kErrBadAddress;
    vfs::File *r, *w;
    if (int64_t e = vfs::pipe(r, w)) return e;
    int64_t rfd = install(r);
    if (rfd < 0) {
        vfs::close(w);
        return rfd;
    }
    int64_t wfd = install(w);
    if (wfd < 0) {
        sys_close(uint64_t(rfd));
        return wfd;
    }
    int32_t fds[2] = {int32_t(rfd), int32_t(wfd)};
    if (!copy_to_user(reinterpret_cast<void*>(out), fds, sizeof(fds))) {
        sys_close(uint64_t(rfd));
        sys_close(uint64_t(wfd));
        return kErrBadAddress;
    }
    return 0;
}

// ---- memory given back, permissions

bool in_alloc_area(Process* p, uint64_t address, uint64_t length) {
    return (address & (kPage - 1)) == 0 && length > 0 && address >= kUserAllocBase && address + length > address &&
           address + length <= p->alloc_next;
}

int64_t sys_unmap(uint64_t address, uint64_t length) {
    Process* p = current();
    length = mem::align_up(length, kPage);
    {
        sync::ScopedLock guard(p->lock);
        if (!in_alloc_area(p, address, length)) return kErrBadArgument;
    }
    for (uint64_t v = address; v < address + length; v += kPage) {
        uint64_t phys = vmm::space_unmap(p->pml4, v);
        if (phys == ~0ull) continue;
        pmm::free_page(pmm::phys_to_virt(phys));
        sync::ScopedLock guard(p->lock);
        --p->user_pages;
    }
    sync::ScopedLock guard(p->lock);
    // Merge with a neighbouring hole, or remember it (when the table is full the range is simply not reused).
    for (uint32_t i = 0; i < p->hole_count; ++i) {
        auto& h = p->holes[i];
        if (h.start + h.size == address) {
            h.size += length;
            return 0;
        }
        if (address + length == h.start) {
            h.start = address;
            h.size += length;
            return 0;
        }
    }
    if (p->hole_count < 32) p->holes[p->hole_count++] = {address, length};
    return 0;
}

int64_t sys_protect(uint64_t address, uint64_t length, uint64_t flags) {
    Process* p = current();
    if ((address & (kPage - 1)) || length == 0 || address + length < address || address + length > kUserEnd) return kErrBadArgument;
    vmm::PageFlags f = vmm::PageFlags::None;
    if (flags & 1) f = f | vmm::PageFlags::Write;
    if (flags & 2) f = f | vmm::PageFlags::Execute;
    for (uint64_t v = address; v < address + mem::align_up(length, kPage); v += kPage)
        if (!vmm::space_protect(p->pml4, v, f)) return kErrBadAddress;
    return 0;
}

// ---- futex: sleep while a user word holds an expected value

struct FutexWaiter {
    Process* process;
    uint64_t address;
    sync::Semaphore wake;
    FutexWaiter* next;
};

sync::SpinLock g_futex_lock;
FutexWaiter* g_futex_waiters = nullptr;

void futex_unlink(FutexWaiter* w) {
    for (FutexWaiter** pp = &g_futex_waiters; *pp; pp = &(*pp)->next) {
        if (*pp == w) {
            *pp = w->next;
            return;
        }
    }
}

int64_t sys_futex_wait(uint64_t address, uint64_t expected, uint64_t timeout_ms) {
    if (address & 3) return kErrBadArgument;
    FutexWaiter w{current(), address, {}, nullptr};
    {
        sync::ScopedLock guard(g_futex_lock);  // a wake cannot slip in between the check and the queueing
        uint32_t value;
        if (!copy_from_user(&value, reinterpret_cast<void*>(address), 4)) return kErrBadAddress;
        if (value != uint32_t(expected)) return kErrBusy;
        w.next = g_futex_waiters;
        g_futex_waiters = &w;
    }
    bool interrupted = false;
    if (timeout_ms == 0) {
        interrupted = !w.wake.wait_interruptible();
    } else {
        uint64_t until = time::uptime_ms() + timeout_ms;
        while (!w.wake.try_wait() && !(interrupted = sched::killed()) && time::uptime_ms() < until) sched::sleep_ms(1);
    }
    sync::ScopedLock guard(g_futex_lock);
    futex_unlink(&w);  // still queued after a timeout or a kill
    return interrupted ? kErrInterrupted : 0;
}

int64_t sys_futex_wake(uint64_t address, uint64_t count) {
    Process* p = current();
    int64_t woken = 0;
    sync::ScopedLock guard(g_futex_lock);
    for (FutexWaiter** pp = &g_futex_waiters; *pp && uint64_t(woken) < count;) {
        FutexWaiter* w = *pp;
        if (w->process == p && w->address == address) {
            *pp = w->next;
            w->wake.signal();
            ++woken;
        } else {
            pp = &w->next;
        }
    }
    return woken;
}

int64_t mkdir_op(const char* p) { return vfs::mkdir(p); }

}  // namespace

int64_t install_file(vfs::File* f) {
    if (!f) return kErrNoMemory;
    if (!current()) {
        vfs::close(f);
        return kErrNoSyscall;
    }
    return install(f);
}

void* socket_of(int64_t fd) {
    if (fd < 0 || !current()) return nullptr;
    vfs::File* f = take(uint64_t(fd));
    void* context = vfs::socket_context(f);
    vfs::close(f);
    return context;
}

namespace {
int64_t remove_op(const char* p) { return vfs::remove(p); }
int64_t chdir_op(const char* p) { return vfs::chdir(p); }

int64_t sys_spawn(uint64_t name, uint64_t args) {
    char n[vfs::kMaxPath], a[512];
    if (copy_string(reinterpret_cast<const void*>(name), n, sizeof(n)) < 0) return kErrBadAddress;
    if (args && copy_string(reinterpret_cast<const void*>(args), a, sizeof(a)) < 0) return kErrBadAddress;
    return spawn(n, args ? a : nullptr);
}

int64_t sys_alloc(uint64_t bytes) {
    Process* p = current();
    if (bytes == 0 || bytes > kUserAllocLimit - kUserAllocBase) return kErrBadArgument;
    uint64_t size = mem::align_up(bytes, kPage);
    uint64_t at = 0;
    {
        sync::ScopedLock guard(p->lock);  // threads of one process allocate concurrently
        for (uint32_t i = 0; i < p->hole_count && !at; ++i) {  // a range given back with unmap, first fit
            if (p->holes[i].size < size) continue;
            at = p->holes[i].start;
            p->holes[i].start += size;
            p->holes[i].size -= size;
            if (p->holes[i].size == 0) p->holes[i] = p->holes[--p->hole_count];
        }
        if (!at) {
            if (p->alloc_next + size > kUserAllocLimit) return kErrNoMemory;
            at = p->alloc_next;
            p->alloc_next += size;
        }
    }
    if (!map_pages(p, at, at + size, vmm::PageFlags::Write)) return kErrNoMemory;  // mapped pages are freed at exit
    return int64_t(at);
}

int64_t sys_process_info(uint64_t index, uint64_t out) {
    ProcessInfo i;
    if (!info(uint32_t(index), i)) return 0;
    return copy_to_user(reinterpret_cast<void*>(out), &i, sizeof(i)) ? 1 : kErrBadAddress;
}

int64_t sys_thread_create(uint64_t rip, uint64_t stack_top, uint64_t arg) {
    Process* p = current();
    if (rip >= kUserEnd || stack_top >= kUserEnd || (stack_top & 15) != 0) return kErrBadArgument;
    {
        sync::ScopedLock guard(g_lock);
        if (p->exiting) return kErrInterrupted;
        if (p->threads >= kMaxThreads) return kErrLimit;
        ++p->threads;
    }
    StartInfo start{p, rip, stack_top, arg, 0};
    uint32_t tid = start_thread(p, start);
    if (!tid) {
        sync::ScopedLock guard(g_lock);
        --p->threads;
        return kErrNoMemory;
    }
    return tid;
}

}  // namespace

}  // namespace neko::proc

extern "C" void neko_syscall_dispatch(neko::proc::SyscallFrame* f) {
    using namespace neko;
    using namespace neko::proc;
    int64_t r = kErrNoSyscall;
    if (f->rip >= kUserEnd) exit_current(kErrBadAddress);  // sysret to a non-canonical rip would fault in ring 0
    switch (Syscall(f->rax)) {
        case Syscall::Exit: exit_current(int64_t(f->rdi));
        case Syscall::Write:
            r = sys_write(f->rdi, f->rsi, f->rdx);
            if (r == kErrBrokenPipe) exit_current(kExitBrokenPipe);  // the reader is gone: like SIGPIPE
            break;
        case Syscall::Read: r = sys_read(f->rdi, f->rsi, f->rdx); break;
        case Syscall::GetPid: r = current()->pid; break;
        case Syscall::Yield: sched::yield(); r = 0; break;
        case Syscall::Sleep: sched::sleep_ms(f->rdi); r = 0; break;
        case Syscall::Uptime: r = int64_t(time::uptime_ms()); break;
        case Syscall::Spawn: r = sys_spawn(f->rdi, f->rsi); break;
        case Syscall::Wait: r = f->rdi > 0xFFFFFFFFu ? kErrNotFound : wait(uint32_t(f->rdi)); break;
        case Syscall::Alloc: r = sys_alloc(f->rdi); break;
        case Syscall::ProcessInfo: r = sys_process_info(f->rdi, f->rsi); break;
        case Syscall::Kill: r = f->rdi > 0xFFFFFFFFu ? kErrNotFound : kill(uint32_t(f->rdi)); break;
        case Syscall::ThreadCreate: r = sys_thread_create(f->rdi, f->rsi, f->rdx); break;
        case Syscall::ThreadExit: sched::exit();
        case Syscall::ThreadId: r = sched::current_id(); break;
        case Syscall::Open: r = sys_open(f->rdi, f->rsi); break;
        case Syscall::Close: r = sys_close(f->rdi); break;
        case Syscall::Seek: r = sys_seek(f->rdi, f->rsi, f->rdx); break;
        case Syscall::Stat: r = sys_stat(f->rdi, f->rsi); break;
        case Syscall::FStat: r = sys_fstat(f->rdi, f->rsi); break;
        case Syscall::ReadDir: r = sys_readdir(f->rdi, f->rsi, f->rdx); break;
        case Syscall::MakeDir: r = sys_path_op(f->rdi, mkdir_op); break;
        case Syscall::Remove: r = sys_path_op(f->rdi, remove_op); break;
        case Syscall::Dup: r = sys_dup(f->rdi); break;
        case Syscall::Dup2: r = sys_dup2(f->rdi, f->rsi); break;
        case Syscall::ChangeDir: r = sys_path_op(f->rdi, chdir_op); break;
        case Syscall::GetCwd: r = sys_getcwd(f->rdi, f->rsi); break;
        case Syscall::Rename: r = sys_rename(f->rdi, f->rsi); break;
        case Syscall::Truncate: r = sys_truncate(f->rdi, f->rsi); break;
        case Syscall::Pipe: r = sys_pipe(f->rdi); break;
        case Syscall::FutexWait: r = sys_futex_wait(f->rdi, f->rsi, f->rdx); break;
        case Syscall::FutexWake: r = sys_futex_wake(f->rdi, f->rsi); break;
        case Syscall::Unmap: r = sys_unmap(f->rdi, f->rsi); break;
        case Syscall::Protect: r = sys_protect(f->rdi, f->rsi, f->rdx); break;
        case Syscall::SetTls:
            if (f->rdi >= kUserEnd) {
                r = kErrBadAddress;
                break;
            }
            sched::current()->fs_base = f->rdi;
#if !NEKO_HOSTED
            __asm__ volatile("wrmsr" : : "c"(0xC0000100u), "a"(uint32_t(f->rdi)), "d"(uint32_t(f->rdi >> 32)));
#endif
            r = 0;
            break;
        default:
            if (f->rax < kMaxSyscalls && g_syscalls[f->rax]) r = g_syscalls[f->rax](f->rdi, f->rsi, f->rdx, f->r10, f->r8, f->r9);
            break;
    }
    f->rax = uint64_t(r);
    check_killed();
}
