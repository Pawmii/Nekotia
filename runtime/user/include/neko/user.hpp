// Runtime for Nekotia user programs (ring 3). Included by code nekoc generates with "target": "user".
//
// A program is a static ELF64 executable linked at 0x400000 (runtime/user/linker.ld). It talks to
// the kernel only through `syscall` (numbers and ABI: runtime/include/neko/process.hpp). The C#
// side of this API is sdk/Nekotia.User.
#pragma once

#include <neko/fmt.hpp>
#include <neko/math.hpp>
#include <neko/types.hpp>

namespace neko::user {

// ---- raw system calls
namespace sys {
inline int64_t call(uint64_t n, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0, uint64_t d = 0, uint64_t e = 0,
                    uint64_t f = 0) {
    int64_t r;
    register uint64_t r10 __asm__("r10") = d;
    register uint64_t r8 __asm__("r8") = e;
    register uint64_t r9 __asm__("r9") = f;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}
}  // namespace sys

// ---- process
NEKO_NORETURN void exit(int64_t code);
uint32_t pid();
void yield();
void sleep_ms(uint64_t ms);
uint64_t uptime_ms();
int64_t spawn(const char* name, const char* args);
int64_t spawn(const uint8_t* name, const uint8_t* args);
int64_t wait(uint32_t pid);
int64_t syscall(uint64_t number, uint64_t a, uint64_t b, uint64_t c);  // for kernel [Syscall] handlers

int64_t kill(uint32_t pid);  // ends it with exit code 137

// ---- threads (share the address space; the process ends when Main returns or anyone calls exit)
using ThreadEntry = void (*)(uint64_t arg);
uint64_t thread_start(ThreadEntry entry, uint64_t arg);  // handle, 0 on failure (64 KiB stack from the heap)
void thread_join(uint64_t handle);                        // waits for it, frees its stack
uint32_t thread_id();
NEKO_NORETURN void thread_exit();

// Spin lock that yields the CPU while it waits.
struct Lock {
    volatile uint32_t value;
    void lock();
    void unlock();
};

int32_t argc();
const uint8_t* arg(int32_t index);  // "" past the end

struct ProcessInfo {  // same layout as neko::proc::ProcessInfo
    uint32_t pid;
    uint32_t parent;
    uint32_t thread_id;
    uint8_t state;  // 0 running, 1 exited
    uint8_t name[24];
    uint64_t memory_kib;
    uint64_t uptime_ms;
    uint32_t threads;
};
bool process_info(uint32_t index, ProcessInfo& out);

// ---- files (paths: absolute or relative to the working directory; results < 0 are errors)
constexpr uint32_t kOpenRead = 1, kOpenWrite = 2, kOpenCreate = 4, kOpenTruncate = 8, kOpenAppend = 16, kOpenExclusive = 32;

struct Stat {  // same layout as neko::vfs::Stat
    uint8_t type;  // 1 file, 2 directory, 3 device
    uint8_t reserved[3];
    uint32_t id;
    uint64_t size;
    uint64_t created_ms;
    uint64_t modified_ms;
};

struct DirEntry {  // same layout as neko::vfs::DirEntry
    uint8_t name[64];
    uint8_t type;
    uint8_t reserved[7];
    uint64_t size;
};

int64_t open(const uint8_t* path, uint32_t flags);  // → fd
int64_t open(const char* path, uint32_t flags);
int64_t close(int32_t fd);
int64_t read(int32_t fd, void* buffer, uint64_t length);
int64_t seek(int32_t fd, int64_t offset, int32_t whence);  // whence: 0 start, 1 current, 2 end
int64_t stat(const uint8_t* path, Stat& out);
int64_t stat(const char* path, Stat& out);
int64_t fstat(int32_t fd, Stat& out);
int64_t readdir(int32_t fd, uint32_t index, DirEntry& out);  // 1, 0 past the end
int64_t mkdir(const uint8_t* path);
int64_t mkdir(const char* path);
int64_t remove(const uint8_t* path);
int64_t remove(const char* path);
int64_t rename(const uint8_t* from, const uint8_t* to);
int64_t chdir(const uint8_t* path);
int64_t chdir(const char* path);
int64_t getcwd(uint8_t* buffer, uint64_t capacity);
int64_t dup(int32_t fd);
int64_t dup2(int32_t fd, int32_t target);
int64_t truncate(int32_t fd, uint64_t size);
int64_t pipe(int32_t& read_fd, int32_t& write_fd);  // read_fd gets what write_fd writes

// ---- network (system calls 40–48, implemented in C# in kernel/Net/Sockets.neko). Sockets are fds.
int64_t tcp_connect(uint32_t ip, uint16_t port);              // → fd
int64_t tcp_listen(uint16_t port);                            // → fd
int64_t tcp_accept(int32_t fd, uint32_t* peer);               // → fd; peer = {ip, port} (may be null)
int64_t tcp_shutdown(int32_t fd);                             // stop sending (FIN), keep reading
int64_t udp_open(uint16_t port);                              // → fd (port 0: any)
int64_t udp_sendto(int32_t fd, uint32_t ip, uint16_t port, const void* data, uint64_t length);
int64_t udp_recvfrom(int32_t fd, void* buffer, uint64_t length, uint32_t* from, uint64_t timeout_ms);
int64_t resolve(const uint8_t* name, uint32_t& ip);           // DNS, "a.b.c.d" or "localhost"
int64_t resolve(const char* name, uint32_t& ip);
int64_t net_config(uint32_t* out4);                           // address, netmask, gateway, DNS

// ---- console (fd 1 = stdout, 2 = stderr, 0 = keyboard — unless redirected)
int64_t write(int32_t fd, const void* data, uint64_t length);
void write(const char* s);
void write(const uint8_t* s);
void write_char(uint8_t c);
void write_codepoint(uint32_t cp);
void write_hex(uint64_t v);
void write_dec(int64_t v);
void write_udec(uint64_t v);
void write_size(uint64_t bytes);
void write_float(double v, int32_t decimals);
void write_error(const char* s);
int64_t read(uint8_t* buffer, uint64_t length);  // from fd 0
int64_t read_line(uint8_t* buffer, uint64_t capacity);  // without '\n', NUL-terminated; -1 at end of input

// ---- memory (first-fit heap over pages from the alloc syscall)
void* alloc(uint64_t size);
void* alloc_zeroed(uint64_t size);
void free(void* block);
uint64_t heap_used();
uint64_t heap_total();

// ---- atomics (sequentially consistent)
inline int64_t atomic_add(int64_t* p, int64_t v) { return __atomic_add_fetch(p, v, __ATOMIC_SEQ_CST); }
inline int64_t atomic_load(int64_t* p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
inline void atomic_store(int64_t* p, int64_t v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
inline bool atomic_cas(int64_t* p, int64_t expected, int64_t desired) {
    return __atomic_compare_exchange_n(p, &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

}  // namespace neko::user

// Shared helpers the SDK maps to (same names as in the kernel runtime).
namespace neko::str {
size_t length(const char* s);
size_t length(const uint8_t* s);
bool equals(const char* a, const char* b);
bool equals(const uint8_t* a, const char* b);
bool equals(const uint8_t* a, const uint8_t* b);
bool starts_with(const uint8_t* s, const char* prefix);
inline uint8_t* bytes(const char* s) { return reinterpret_cast<uint8_t*>(const_cast<char*>(s)); }
}  // namespace neko::str

// What nekoc lowers `throw`, Debug.Assert and Debug.Panic to: a message on stderr and exit code 134.
namespace neko::crash {
NEKO_NORETURN void unhandled_exception(const char* type, const char* message, const char* file, int32_t line,
                                       const char* function);
NEKO_NORETURN void assert_failed(const char* message, const char* file, int32_t line, const char* function);
NEKO_NORETURN void panic_at(const char* message, const char* file, int32_t line, const char* function);
}  // namespace neko::crash
