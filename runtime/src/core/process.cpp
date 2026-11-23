// User processes: program modules, the ELF loader, the process table, exit/wait and the
// system call table (see neko/process.hpp for the ABI and the address-space layout).
//
// Life of a process:
//   spawn  — find the module, check the ELF, build a fresh address space (image + stack with
//            argc/argv), start a kernel thread bound to it (sched::spawn_for)
//   start  — that thread loads a clean FPU state and drops to ring 3 (arch::enter_user)
//   run    — system calls arrive through neko_syscall_dispatch; CPU exceptions in ring 3 end the
//            process (user_exception) instead of stopping the kernel
//   exit   — the thread ends; once it is off its CPU the scheduler calls thread_freed, which
//            frees the address space and wakes the parent blocked in wait()
//   wait   — the parent collects the exit code and the Process record is freed
#include <neko/process.hpp>
#include <neko/runtime.hpp>

namespace neko::proc {
namespace {

// ---------------------------------------------------------------- modules

constexpr uint32_t kMaxModules = 32;

struct Module {
    char name[24];
    const uint8_t* data;
    uint64_t size;
};

Module g_modules[kMaxModules];
uint32_t g_module_count = 0;

const Module* find_module(const char* name) {
    for (uint32_t i = 0; i < g_module_count; ++i)
        if (str::equals(g_modules[i].name, name)) return &g_modules[i];
    return nullptr;
}

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

bool load_image(Process* p, const uint8_t* data) {
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
    p->entry = h->entry;
    return true;
}

// Stack: [argv[0] … argv[n-1] strings][padding][argv pointers, NULL] ← rsp (16-byte aligned).
bool build_stack(Process* p, const char* name, const char* args) {
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

    p->argc = argc;
    p->argv = argv_at;
    p->user_rsp = argv_at;
    p->alloc_next = kUserAllocBase;
    return true;
}

// ---------------------------------------------------------------- process thread

alignas(16) uint8_t g_clean_fpu[512];

// First code of a process thread (kernel mode, already on the process's page tables).
void process_main(uint64_t arg) {
    auto* p = reinterpret_cast<Process*>(arg);
    // Nothing of the kernel's FPU/SSE state may leak into the program.
    __asm__ volatile("fxrstor %0" : : "m"(g_clean_fpu));
    arch::enter_user(p->entry, p->user_rsp, p->argc, p->argv);
}

void release(Process* p) {
    vmm::space_destroy(p->pml4);
    heap::free(p);
}

}  // namespace

// ---------------------------------------------------------------- modules

void add_module(const char* path, const void* data, uint64_t size) {
    if (g_module_count == kMaxModules || !path) return;
    const char* base = path;
    for (const char* c = path; *c; ++c)
        if (*c == '/') base = c + 1;
    Module& m = g_modules[g_module_count++];
    copy_name(reinterpret_cast<uint8_t*>(m.name), base);
    m.data = static_cast<const uint8_t*>(data);
    m.size = size;
}

uint32_t module_count() { return g_module_count; }
const char* module_name(uint32_t i) { return i < g_module_count ? g_modules[i].name : ""; }
uint64_t module_size(uint32_t i) { return i < g_module_count ? g_modules[i].size : 0; }

// ---------------------------------------------------------------- processes

Process* current() {
    sched::Thread* t = sched::current();
    return t ? t->process : nullptr;
}

int64_t spawn(const char* name, const char* args, bool detached) {
    if (!name || !*name) return kErrBadArgument;
    if (!arch::user_mode_available()) return kErrNoSyscall;
    const Module* m = find_module(name);
    if (!m) return kErrNotFound;
    if (!valid_elf(m->data, m->size)) return kErrBadExecutable;

    auto* p = static_cast<Process*>(heap::alloc_zeroed(sizeof(Process)));
    if (!p) return kErrNoMemory;
    p->pml4 = vmm::space_create();
    if (!p->pml4) {
        heap::free(p);
        return kErrNoMemory;
    }
    p->pml4_phys = pmm::virt_to_phys(p->pml4);
    if (!load_image(p, m->data) || !build_stack(p, m->name, args)) {
        release(p);
        return kErrNoMemory;
    }
    copy_name(p->name, m->name);
    Process* parent = current();
    p->parent = parent ? parent->pid : 0;
    p->detached = detached;
    p->started_ms = time::uptime_ms();

    static bool fpu_ready = false;
    if (!fpu_ready) {
        mem::set(g_clean_fpu, 0, sizeof(g_clean_fpu));
        g_clean_fpu[0] = 0x7F;  // FCW 0x037F: x87 exceptions masked, extended precision
        g_clean_fpu[1] = 0x03;
        reinterpret_cast<uint32_t*>(g_clean_fpu)[6] = 0x1F80;  // MXCSR: SSE exceptions masked
        fpu_ready = true;
    }

    {
        sync::ScopedLock guard(g_lock);
        p->pid = g_next_pid++;
        p->next = g_processes;
        g_processes = p;
        ++g_count;
    }
    uint32_t pid = p->pid;
    uint32_t tid = sched::spawn_for(p, m->name, &process_main, reinterpret_cast<uint64_t>(p));
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
    {
        sync::ScopedLock guard(g_lock);
        p = find_locked(pid);
        if (!p) return kErrNotFound;
        if (p->parent != caller || p->detached || p->waited) return kErrNotChild;
        p->waited = true;
    }
    p->exited.wait();
    {
        sync::ScopedLock guard(g_lock);
        unlink_locked(p);
    }
    int64_t code = p->exit_code;
    heap::free(p);
    return code;
}

void exit_current(int64_t code) {
    Process* p = current();
    if (!p) panic("proc::exit_current called from a kernel thread");
    p->exit_code = code;
    sched::exit();
}

void thread_freed(sched::Thread* t) {
    Process* p = t->process;
    vmm::space_destroy(p->pml4);
    p->pml4 = nullptr;
    p->user_pages = 0;
    bool free_now;
    {
        sync::ScopedLock guard(g_lock);
        p->state = State::Exited;
        free_now = p->detached;
        if (free_now) unlink_locked(p);
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

// ---------------------------------------------------------------- system calls

bool register_syscall(uint32_t number, SyscallHandler handler) {
    if (number < kFirstFreeSyscall && number != uint32_t(Syscall::Read)) return false;  // runtime-owned
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

int64_t copy_string(const void* user, char* out, uint64_t capacity) {
    auto* s = static_cast<const char*>(user);
    for (uint64_t i = 0; i < capacity; ++i) {
        if ((i == 0 || ((reinterpret_cast<uint64_t>(s) + i) & (kPage - 1)) == 0) && !check_read(s + i, 1))
            return kErrBadAddress;
        out[i] = s[i];
        if (!out[i]) return int64_t(i);
    }
    return kErrBadArgument;  // too long
}

namespace {

int64_t sys_write(uint64_t fd, uint64_t buffer, uint64_t length) {
    if (fd != 1 && fd != 2) return kErrBadArgument;
    if (length > (1u << 20)) length = 1u << 20;
    auto* src = reinterpret_cast<const char*>(buffer);
    if (!check_read(src, length)) return kErrBadAddress;
    char chunk[257];
    for (uint64_t done = 0; done < length;) {
        uint64_t n = length - done < 256 ? length - done : 256;
        mem::copy(chunk, src + done, n);
        chunk[n] = 0;
        term::write(chunk);
        done += n;
    }
    return int64_t(length);
}

int64_t sys_spawn(uint64_t name, uint64_t args) {
    char n[24], a[512];
    if (copy_string(reinterpret_cast<const void*>(name), n, sizeof(n)) < 0) return kErrBadAddress;
    if (args && copy_string(reinterpret_cast<const void*>(args), a, sizeof(a)) < 0) return kErrBadAddress;
    return spawn(n, args ? a : nullptr);
}

int64_t sys_alloc(uint64_t bytes) {
    Process* p = current();
    if (bytes == 0 || bytes > kUserAllocLimit - kUserAllocBase) return kErrBadArgument;
    uint64_t size = mem::align_up(bytes, kPage);
    if (p->alloc_next + size > kUserAllocLimit) return kErrNoMemory;
    uint64_t at = p->alloc_next;
    if (!map_pages(p, at, at + size, vmm::PageFlags::Write)) return kErrNoMemory;  // mapped pages are freed at exit
    p->alloc_next += size;
    return int64_t(at);
}

int64_t sys_process_info(uint64_t index, uint64_t out) {
    if (!check_write(reinterpret_cast<void*>(out), sizeof(ProcessInfo))) return kErrBadAddress;
    ProcessInfo i;
    if (!info(uint32_t(index), i)) return 0;
    mem::copy(reinterpret_cast<void*>(out), &i, sizeof(i));
    return 1;
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
        case Syscall::Write: r = sys_write(f->rdi, f->rsi, f->rdx); break;
        case Syscall::GetPid: r = current()->pid; break;
        case Syscall::Yield: sched::yield(); r = 0; break;
        case Syscall::Sleep: sched::sleep_ms(f->rdi); r = 0; break;
        case Syscall::Uptime: r = int64_t(time::uptime_ms()); break;
        case Syscall::Spawn: r = sys_spawn(f->rdi, f->rsi); break;
        case Syscall::Wait: r = f->rdi > 0xFFFFFFFFu ? kErrNotFound : wait(uint32_t(f->rdi)); break;
        case Syscall::Alloc: r = sys_alloc(f->rdi); break;
        case Syscall::ProcessInfo: r = sys_process_info(f->rdi, f->rsi); break;
        default:
            if (f->rax < kMaxSyscalls && g_syscalls[f->rax]) r = g_syscalls[f->rax](f->rdi, f->rsi, f->rdx, f->r10, f->r8, f->r9);
            break;
    }
    f->rax = uint64_t(r);
}
