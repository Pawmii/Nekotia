// User processes (core/process.cpp, arch/x86_64/usermode.cpp, arch/x86_64/syscall.S).
//
// A process is one address space + one thread running in ring 3. Programs are static ELF64
// executables handed to the kernel as Limine modules (/boot/bin/* on the ISO) and built from C#
// by nekoc with "target": "user" (see user/ and runtime/user/).
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

namespace neko::proc {

enum class Syscall : uint32_t {
    Exit = 0,      // exit(code)                          never returns
    Write = 1,     // write(fd, buffer, length)           → bytes written (fd 1 = stdout, 2 = stderr)
    Read = 2,      // read(fd, buffer, length)            → bytes read (fd 0 = keyboard; implemented in C#)
    GetPid = 3,    // getpid()                            → process id
    Yield = 4,     // yield()
    Sleep = 5,     // sleep(milliseconds)
    Uptime = 6,    // uptime()                            → milliseconds since boot
    Spawn = 7,     // spawn(name, arguments)              → pid of the new process
    Wait = 8,      // wait(pid)                           → its exit code (only for its parent)
    Alloc = 9,     // alloc(bytes)                        → address of fresh zeroed RW pages
    ProcessInfo = 10,  // process_info(index, ProcessInfo*) → 1, or 0 past the end
};
constexpr uint32_t kMaxSyscalls = 64;
constexpr uint32_t kFirstFreeSyscall = 32;  // numbers below are reserved for the runtime

// Errors are negative results.
enum Error : int64_t {
    kErrBadArgument = -1,
    kErrNotFound = -2,
    kErrNoMemory = -3,
    kErrBadAddress = -4,
    kErrNoSyscall = -5,
    kErrBadExecutable = -6,
    kErrNotChild = -7,
};

// Exit code of a process killed by a CPU exception: 128 + vector (like a Unix signal number).
constexpr int64_t kKilledBase = 128;

constexpr uint64_t kUserImageBase = 0x400000;
constexpr uint64_t kUserAllocBase = 0x0000100000000000ull;
constexpr uint64_t kUserAllocLimit = 0x0000100000000000ull + (256ull << 20);  // 256 MiB per process
constexpr uint64_t kUserStackTop = 0x00007fffffff0000ull;
constexpr uint64_t kUserStackSize = 256 * 1024;
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
    uint32_t thread_id;
    uint64_t started_ms;
    uint64_t entry, user_rsp, argc, argv;  // where the thread enters ring 3
    sync::Semaphore exited;  // signalled once the address space is gone
    Process* next;
};

// ---- program modules (Limine modules: /boot/bin/NAME)
void add_module(const char* path, const void* data, uint64_t size);
uint32_t module_count();
const char* module_name(uint32_t index);
uint64_t module_size(uint32_t index);

// ---- processes
// Starts program `name` with `args` (space-separated, may be null). Returns the pid or an Error.
// `detached`: nobody will wait for it.
int64_t spawn(const char* name, const char* args, bool detached = false);
inline int64_t spawn(const uint8_t* name, const uint8_t* args, bool detached = false) {
    return spawn(reinterpret_cast<const char*>(name), reinterpret_cast<const char*>(args), detached);
}
// Waits until process `pid` exits and returns its exit code (frees the process).
int64_t wait(uint32_t pid);
Process* current();  // nullptr in kernel threads
inline uint32_t current_pid() { return current() ? current()->pid : 0; }
NEKO_NORETURN void exit_current(int64_t code);

struct ProcessInfo {
    uint32_t pid;
    uint32_t parent;
    uint32_t thread_id;
    State state;
    uint8_t name[24];
    uint64_t memory_kib;  // user pages
    uint64_t uptime_ms;   // since it started
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
}

// Address spaces (arch/x86_64/vmm.cpp; stubs in host mode).
namespace neko::vmm {
uint64_t* space_create();                       // new PML4 sharing the kernel half; nullptr when out of memory
void space_destroy(uint64_t* pml4);             // frees every user page and table (must not be active anywhere)
bool space_map(uint64_t* pml4, uint64_t virt, uint64_t phys, PageFlags flags);  // user page; false if taken / OOM
void space_add_flags(uint64_t* pml4, uint64_t virt, PageFlags flags);  // widen a mapped page (segments sharing a page)
uint64_t space_translate(uint64_t* pml4, uint64_t virt);  // physical address or ~0
bool space_check(uint64_t* pml4, uint64_t virt, uint64_t length, bool write);  // user-accessible?
}
