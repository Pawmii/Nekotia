// Threads, the scheduler and synchronisation primitives (core/sched.cpp, core/sync.cpp).
//
// Kernel threads are preempted by the 1 kHz timer (10 ms slices) and picked by priority
// (High > Normal > Low, round-robin inside a priority). Each thread runs on its own
// stack with a guard page below it. The FPU/SSE state is saved on every switch.
#pragma once

#include <neko/types.hpp>

namespace neko::sync {
struct Mutex;

// Bare test-and-set lock: no interrupt handling, not recursive. The caller manages IF.
struct RawLock {
    volatile uint32_t value;
    void acquire() {
        while (__atomic_exchange_n(&value, 1u, __ATOMIC_ACQUIRE))
            while (__atomic_load_n(&value, __ATOMIC_RELAXED)) __builtin_ia32_pause();
    }
    void release() { __atomic_store_n(&value, 0u, __ATOMIC_RELEASE); }
};
}

namespace neko::sched {

enum class Priority : uint8_t { High = 0, Normal = 1, Low = 2 };
enum class State : uint8_t { Ready = 0, Running = 1, Sleeping = 2, Blocked = 3, Dead = 4 };

struct Thread {
    uint64_t rsp;  // saved stack pointer (must stay first: used by switch.S callers)
    uint32_t id;
    State state;
    Priority priority;
    uint8_t name[24];
    uint64_t stack_top;
    uint64_t stack_size;
    uint64_t wake_at;     // Sleeping: uptime in ms
    uint64_t cpu_ms;      // time spent running
    uint64_t switches;    // times switched in
    uint32_t cpu;         // CPU it last ran on
    const char* blocked_on;     // what a Blocked thread waits for (for ps and deadlock reports)
    sync::Mutex* waiting_for;   // mutex a Blocked thread waits for (deadlock detection)
    Thread* next;         // run queue / wait queue link
    Thread* all_next;     // list of every thread
};

using Entry = void (*)(uint64_t arg);

// Adopts the current flow of execution as thread #1 ("kernel") and creates the idle thread.
void init(uint64_t current_stack_top, uint64_t current_stack_size);
bool running();

// Starts a thread; returns its id (0 when out of memory). The thread ends when `entry` returns.
uint32_t spawn(const char* name, Entry entry, uint64_t arg = 0, Priority priority = Priority::Normal);

void yield();
void sleep_ms(uint64_t ms);
NEKO_NORETURN void exit();

Thread* current();
uint32_t current_id();
const char* current_name();

// Low level, for synchronisation primitives. Interrupts must be disabled by the caller.
// Marks the current thread Blocked and runs something else; `guard` (the primitive's own lock,
// held by the caller) is released only once the scheduler lock is taken, so no wake-up is lost.
void block(const char* reason, sync::RawLock* guard);
void wake(Thread* t);  // Blocked → Ready (callable from interrupt handlers and other CPUs)

// Per-CPU startup: adopts the calling flow as this CPU's idle thread (APs), then run_idle().
void cpu_online();
NEKO_NORETURN void run_idle();
void thread_started();  // internal: first code of every new thread (releases the scheduler lock)

// Called by the timer interrupt (after EOI) and at the end of every IRQ.
void tick();
void preempt_if_needed();

// Statistics.
struct ThreadInfo {
    uint32_t id;
    State state;
    Priority priority;
    uint8_t name[24];
    uint64_t cpu_ms;
    uint64_t switches;
    uint64_t stack_kib;
    uint32_t cpu;
    const char* blocked_on;  // "" unless Blocked/Sleeping
};
uint32_t thread_count();
bool thread_info(uint32_t index, ThreadInfo& out);
uint64_t context_switches();
uint64_t idle_ms();
bool has_sleepers();  // host mode: "can anything still wake up?"

}  // namespace neko::sched

namespace neko::sync {

// Busy-waiting lock that also disables interrupts on this CPU. Recursive on the same CPU,
// so the heap may call into the VMM (which allocates) while holding it.
struct SpinLock {
    volatile uint32_t locked;
    volatile uint32_t owner;  // CPU index + 1, 0 = free
    uint32_t depth;
    uint64_t saved_flags;
    void lock();
    void unlock();
};

struct ScopedLock {
    SpinLock& l;
    explicit ScopedLock(SpinLock& lock) : l(lock) { l.lock(); }
    ~ScopedLock() { l.unlock(); }
};

// Sleeping lock with ownership. Locking it twice from the same thread, or a cycle of threads
// waiting for each other's mutexes, stops the kernel with a DEADLOCK report.
struct Mutex {
    RawLock guard;
    sched::Thread* owner;
    sched::Thread* head;
    sched::Thread* tail;
    const char* label;
    void lock();
    bool try_lock();
    void unlock();
    void set_name(const char* name) { label = name; }
    bool is_locked() const { return owner != nullptr; }
};

// Counting semaphore. signal() may be called from interrupt handlers.
struct Semaphore {
    RawLock guard;
    int64_t count;
    sched::Thread* head;
    sched::Thread* tail;
    void wait();
    bool try_wait();
    void signal();
    int64_t value() const { return count; }
};

}  // namespace neko::sync
