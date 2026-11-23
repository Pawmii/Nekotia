// Threads, the scheduler and synchronisation primitives (core/sched.cpp, core/sync.cpp).
//
// Kernel threads are preempted by the 1 kHz timer (10 ms slices) and picked by priority
// (High > Normal > Low, round-robin inside a priority). Each thread runs on its own
// stack with a guard page below it. The FPU/SSE state is saved on every switch.
#pragma once

#include <neko/types.hpp>

namespace neko::sync {
struct Mutex;
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
void block(const char* reason);  // current thread → Blocked, run something else
void wake(Thread* t);            // Blocked → Ready (callable from interrupt handlers)

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
    const char* blocked_on;  // nullptr unless Blocked/Sleeping
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
    int64_t count;
    sched::Thread* head;
    sched::Thread* tail;
    void wait();
    bool try_wait();
    void signal();
    int64_t value() const { return count; }
};

}  // namespace neko::sync
