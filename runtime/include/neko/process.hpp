// User processes (core/process.cpp, arch/x86_64/usermode.cpp, arch/x86_64/syscall.S).
//
// A process is one address space + one or more threads running in ring 3, a table of open files
// (fds 0–2 are inherited from the parent; the kernel gives its own children /dev/console) and a
// working directory. Programs are static ELF64 executables in the VFS (/bin/NAME, from the initrd)
// built from C# by nekoc with "target": "user" (see user/ and runtime/user/).
//
// User address space (lower half; the kernel half is shared by every process):
//   0x0000000000400000  program image (PT_LOAD segments, R / RW / RX, NX where not executable)
//   0x0000100000000000  memory from the alloc syscall, grows upwards
//   0x00007fffffff0000  top of the 256 KiB stack (argc/argv strings at the very top)
//
// System calls: `syscall` with the number in rax and arguments in rdi, rsi, rdx, r10, r8, r9;
// the result comes back in rax (negative = error). rcx and r11 are clobbered, everything else
// (including SSE registers) is preserved. See SyscallNumber below; the kernel's C# code can add
// its own with [Syscall(n)].
#pragma once

#include <neko/memory.hpp>
#include <neko/sched.hpp>

namespace neko::crash {
struct CpuState;
}

namespace neko::vfs {
struct Node;
struct File;
}

namespace neko::proc {

enum class Syscall : uint32_t {
    Exit = 0,      // exit(code)                          never returns
    Write = 1,     // write(fd, buffer, length)           → bytes written
    Read = 2,      // read(fd, buffer, length)            → bytes read, 0 at the end (console: one line)
    GetPid = 3,    // getpid()                            → process id
    Yield = 4,     // yield()
    Sleep = 5,     // sleep(milliseconds)
    Uptime = 6,    // uptime()                            → milliseconds since boot
    Spawn = 7,     // spawn(program, arguments)           → pid; program = /bin/NAME or a path with '/' 
    Wait = 8,      // wait(pid)                           → its exit code (only for its parent)
    Alloc = 9,     // alloc(bytes)                        → address of fresh zeroed RW pages
    ProcessInfo = 10,  // process_info(index, ProcessInfo*) → 1, or 0 past the end
    Kill = 11,         // kill(pid)                          → 0; the process ends with exit code 137
    ThreadCreate = 12, // thread_create(rip, stack_top, arg) → thread id; starts at rip with rdi = arg
    ThreadExit = 13,   // thread_exit()                      never returns; the last thread ends the process
    ThreadId = 14,     // thread_id()                        → id of the calling thread
    // 15 is unused (was program_info: programs are files in /bin now)
    SetTls = 31,       // set_tls(address)                   → 0; FS base of the calling thread (%fs:0 = its TLS block)
    FutexWait = 32,    // futex_wait(uint32_t* a, expected, timeout_ms; 0 = none) → 0 when woken, kErrBusy if *a != expected,
                       //                                    0 after the timeout too (check your condition), kErrInterrupted
    FutexWake = 33,    // futex_wake(uint32_t* a, count)     → threads woken
    Unmap = 34,        // unmap(address, length)             → 0; pages from alloc go back to the kernel
    Protect = 35,      // protect(address, length, flags)    → 0; flags: 1 write, 2 execute (always readable)
    Open = 16,         // open(path, flags)                  → fd (flags: neko/vfs.hpp kRead…)
    Close = 17,        // close(fd)                          → 0
    Seek = 18,         // seek(fd, offset, whence)           → new offset
    Stat = 19,         // stat(path, Stat*)                  → 0
    FStat = 20,        // fstat(fd, Stat*)                   → 0
    ReadDir = 21,      // readdir(fd, index, DirEntry*)      → 1, or 0 past the end
    MakeDir = 22,      // mkdir(path)                        → 0
    Remove = 23,       // remove(path)                       → 0 (file or empty directory)
    Dup = 24,          // dup(fd)                            → new fd sharing the open file
    Dup2 = 25,         // dup2(fd, target)                   → target (closed first if open)
    ChangeDir = 26,    // chdir(path)                        → 0
    GetCwd = 27,       // getcwd(buffer, capacity)           → length (NUL-terminated)
    Rename = 28,       // rename(from, to)                   → 0
    Truncate = 29,     // truncate(fd, size)                 → 0
    Pipe = 30,         // pipe(int32_t fds[2])               → 0; fds[0] reads what fds[1] writes
    Poll = 36,         // poll(PollFd*, count, timeout_ms)   → fds ready (0 after the timeout; ~0 = no timeout)
    Mmap = 37,         // mmap(fd, length, flags)            → address; flags: 1 write. Shared memory, /dev/fb0…
                       //                                    (unmap(address, length) removes the whole mapping)
    Ioctl = 38,        // ioctl(fd, request, arg)            → device-specific (neko/display.hpp, neko/input.hpp)
    ShmCreate = 39,    // shm_create(bytes)                  → fd of fresh zeroed shared memory (neko/ipc.hpp)
    // 40–63: C# [Syscall] handlers (kernel/…)
    IpcListen = 64,    // ipc_listen(name)                   → fd that accepts connections to `name`
    IpcConnect = 65,   // ipc_connect(name)                  → fd of a new channel to that listener
    IpcAccept = 66,    // ipc_accept(listen_fd)              → fd of the server end of the next connection (blocks)
    IpcSend = 67,      // ipc_send(fd, data, length, attach_fd; -1 = none) → length; kErrBusy when the queue is full
    IpcRecv = 68,      // ipc_recv(fd, buffer, capacity, int32_t* fd_out) → length, 0 at the end; *fd_out = the
                       //                                    file that came along as a new fd, or -1
};
constexpr uint32_t kMaxSyscalls = 64;       // size of the C# handler table
constexpr uint32_t kFirstFreeSyscall = 40;  // numbers below (and from 64) are the runtime's (C# [Syscall]: 40–63)

// poll() entry (same layout in neko/user.hpp).
struct PollFd {
    int32_t fd;
    uint16_t events;   // vfs::kPollIn | kPollOut
    uint16_t revents;  // what is ready, plus kPollHup; 0x8000 = bad fd
};
constexpr uint32_t kMaxPollFds = 64;

// Errors are negative results.
enum Error : int64_t {
    kErrBadArgument = -1,
    kErrNotFound = -2,
    kErrNoMemory = -3,
    kErrBadAddress = -4,
    kErrNoSyscall = -5,
    kErrBadExecutable = -6,
    kErrNotChild = -7,
    kErrInterrupted = -8,  // the caller is being killed
    kErrLimit = -9,        // too many threads
    // files (neko/vfs.hpp)
    kErrExists = -10,
    kErrNotDirectory = -11,
    kErrIsDirectory = -12,
    kErrNotEmpty = -13,
    kErrBadFd = -14,
    kErrTooManyFiles = -15,
    kErrNameTooLong = -16,
    kErrBusy = -17,        // e.g. moving a directory into itself
    kErrAccess = -18,      // writing a file opened read-only, reading a write-only one
    kErrNotSupported = -19,
    kErrIo = -20,          // the device reported an error
    kErrBrokenPipe = -21,  // writing to a pipe nobody reads any more
};

// Exit code of a process killed by a CPU exception: 128 + vector (like a Unix signal number).
constexpr int64_t kKilledBase = 128;
constexpr int64_t kExitInterrupted = 130;  // Ctrl+C
constexpr int64_t kExitKilled = 137;       // kill
constexpr int64_t kExitBrokenPipe = 141;   // wrote to a pipe without readers (like SIGPIPE)
constexpr uint32_t kMaxThreads = 64;       // per process
constexpr uint32_t kMaxFiles = 32;         // open file descriptors per process

constexpr uint64_t kUserImageBase = 0x400000;
constexpr uint64_t kUserAllocBase = 0x0000100000000000ull;
constexpr uint64_t kUserAllocLimit = 0x0000100000000000ull + (256ull << 20);  // 256 MiB per process
constexpr uint64_t kUserMapBase = 0x0000200000000000ull;              // mmap()ed objects
constexpr uint64_t kUserMapLimit = kUserMapBase + (64ull << 30);
constexpr uint32_t kMaxMappings = 32;      // per process
constexpr uint64_t kUserStackTop = 0x00007fffffff0000ull;
constexpr uint64_t kUserStackSize = 1024 * 1024;   // reserved; pages appear when touched (demand paging)
constexpr uint64_t kUserStackMapped = 16 * 1024;   // mapped at start (argv lives there)
constexpr uint64_t kUserEnd = 0x0000800000000000ull;  // first non-canonical address

enum class State : uint8_t { Running = 0, Exited = 1 };

struct Process {
    uint32_t pid;
    uint32_t parent;     // pid of the parent process; 0 = started by the kernel
    State state;
    bool waited;         // someone is (or was) waiting for it
    bool detached;       // nobody will wait: freed as soon as it exits
    uint8_t name[24];
    int64_t exit_code;
    uint64_t* pml4;      // its page tables (HHDM address); the upper half is the kernel's
    uint64_t pml4_phys;
    uint64_t alloc_next; // next address for the alloc syscall
    uint64_t user_pages; // pages mapped for it (image, stack, allocations)
    uint32_t thread_id;  // main thread
    uint32_t threads;    // threads not yet freed; the last one tears the address space down
    bool exiting;        // exit/kill/fault happened: exit_code is final, every thread is being ended
    uint64_t started_ms;
    sync::SpinLock lock; // alloc_next, user_pages, files, holes
    struct { uint64_t start, size; } holes[32];  // unmapped ranges below alloc_next, reused by alloc
    uint32_t hole_count;
    vfs::File* files[kMaxFiles];  // file descriptors
    struct { uint64_t start, size; vfs::File* file; } maps[kMaxMappings];  // mmap()s, sorted by start; each holds its file
    uint32_t map_count;
    vfs::Node* cwd;      // working directory
    sync::Semaphore exited;  // signalled once the address space is gone
    Process* next;
};

// ---- processes
// Starts program `name` (/bin/NAME, or a path when it contains '/') with `args` (space-separated,
// may be null). Returns the pid or an Error. `detached`: nobody will wait for it.
int64_t spawn(const char* name, const char* args, bool detached = false);
inline int64_t spawn(const uint8_t* name, const uint8_t* args, bool detached = false) {
    return spawn(reinterpret_cast<const char*>(name), reinterpret_cast<const char*>(args), detached);
}
// Waits until process `pid` exits and returns its exit code (frees the process).
int64_t wait(uint32_t pid);
Process* current();  // nullptr in kernel threads
inline uint32_t current_pid() { return current() ? current()->pid : 0; }
NEKO_NORETURN void exit_current(int64_t code);  // ends the whole process (every thread)
int64_t kill(uint32_t pid, int64_t code = kExitKilled);  // 0 or kErrNotFound; callable from IRQ handlers
// Ctrl+C: ends the foreground process (the one the console's owner is waiting for) with code 130.
// Returns false when the kernel shell itself is in the foreground. Callable from IRQ handlers.
bool interrupt_foreground();
uint32_t foreground();  // pid, 0 = kernel
// Called before returning to ring 3: ends the current thread if it was killed.
void check_killed();
// Demand paging: `address` lies in memory the current process reserved but never touched (the alloc area
// below alloc_next and outside the holes, or its stack): a zeroed page is mapped there and true returned —
// the faulting instruction simply runs again. False: a real fault.
bool demand_page(uint64_t address);
uint64_t demand_faults();  // pages mapped this way since boot

struct ProcessInfo {
    uint32_t pid;
    uint32_t parent;
    uint32_t thread_id;
    State state;
    uint8_t name[24];
    uint64_t memory_kib;  // user pages
    uint64_t uptime_ms;   // since it started
    uint32_t threads;
};
uint32_t count();
bool info(uint32_t index, ProcessInfo& out);

// Called by the scheduler once an exited thread is off its CPU (outside the scheduler lock).
void thread_freed(sched::Thread* t);
// CPU exception raised in ring 3: report it and end the process.
NEKO_NORETURN void user_exception(const crash::CpuState* state);
void set_fault_reports(bool on);  // the quiet boot self test kills programs on purpose

// ---- system calls
using SyscallHandler = int64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
bool register_syscall(uint32_t number, SyscallHandler handler);  // false if taken or out of range

// User pointers: true when [address, address + length) is mapped for the current process
// (writable too, for check_write). Kernel threads have no user memory.
bool check_read(const void* address, uint64_t length);
bool check_write(const void* address, uint64_t length);
// Copies a NUL-terminated user string into `out` (at most `capacity` bytes incl. NUL). Length or Error.
int64_t copy_string(const void* user, char* out, uint64_t capacity);
inline int64_t copy_string(const void* user, uint8_t* out, uint64_t capacity) { return copy_string(user, reinterpret_cast<char*>(out), capacity); }
// The only way the kernel touches user memory (SMAP is on where the CPU has it): range-checked,
// and a page fault inside the copy returns false instead of stopping the kernel.
bool copy_from_user(void* dst, const void* user_src, uint64_t length);
bool copy_to_user(void* user_dst, const void* src, uint64_t length);

// File descriptors of the calling process, for system calls written in C#.
int64_t install_file(vfs::File* f);  // the new fd (closes f when the table is full)
void* socket_of(int64_t fd);         // vfs::socket_context of that fd, nullptr if it is not a socket

// Register state saved by the syscall entry (syscall.S): the same order as crash::CpuState
// without vector / error code.
struct SyscallFrame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t rip, cs, rflags, rsp, ss;
};

}  // namespace neko::proc

// Platform hooks (arch/x86_64/usermode.cpp; stubs in host mode).
namespace neko::arch {
// Enters ring 3 at `rip` with stack `rsp`, rdi = arg0, rsi = arg1. Never returns.
NEKO_NORETURN void enter_user(uint64_t rip, uint64_t rsp, uint64_t arg0, uint64_t arg1);
bool user_mode_available();
void enable_smep_smap();  // per CPU, once our page tables are active
bool smep_enabled();
bool smap_enabled();
}

// Address spaces (arch/x86_64/vmm.cpp; stubs in host mode).
namespace neko::vmm {
uint64_t* space_create();                       // new PML4 sharing the kernel half; nullptr when out of memory
void space_destroy(uint64_t* pml4);             // frees every user page and table (must not be active anywhere)
bool space_map(uint64_t* pml4, uint64_t virt, uint64_t phys, PageFlags flags);  // user page; false if taken / OOM
void space_add_flags(uint64_t* pml4, uint64_t virt, PageFlags flags);  // widen a mapped page (segments sharing a page)
uint64_t space_unmap(uint64_t* pml4, uint64_t virt);          // the frame it had (not freed), or ~0
// Unmaps `pages` pages from `virt` with one TLB shootdown for all of them, then calls each(frame, ctx)
// for every page that was mapped (frames are not freed; Shared pages report frame | 1 so the caller can
// tell them apart). Returns how many were mapped.
uint64_t space_unmap_range(uint64_t* pml4, uint64_t virt, uint64_t pages, void (*each)(uint64_t frame, void* ctx), void* ctx);
bool space_protect(uint64_t* pml4, uint64_t virt, PageFlags flags);  // replace the permissions of a mapped user page
uint64_t space_translate(uint64_t* pml4, uint64_t virt);  // physical address or ~0
bool space_check(uint64_t* pml4, uint64_t virt, uint64_t length, bool write);  // user-accessible?
}
