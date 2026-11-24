// Threads, the scheduler and synchronisation primitives (core/sched.cpp, core/sync.cpp).
//
// Kernel threads are preempted by the 1 kHz timer (10 ms slices) and picked by priority
// (High > Normal > Low, round-robin inside a priority). Each thread runs on its own
// stack with a guard page below it. The FPU/SSE state is saved on every switch.
#pragma once

#include <neko/smp.hpp>
#include <neko/types.hpp>

namespace neko::sync {
struct Mutex;
struct Semaphore;

// Bare test-and-set lock: no interrupt handling, not recursive. The caller manages IF.
struct RawLock {
    volatile uint32_t value;
    void acquire() {
        while (__atomic_exchange_n(&value, 1u, __ATOMIC_ACQUIRE))
            while (__atomic_load_n(&value, __ATOMIC_RELAXED)) {
                smp::tlb_service();
                __builtin_ia32_pause();
            }
    }
    void release() { __atomic_store_n(&value, 0u, __ATOMIC_RELEASE); }
};
}

namespace neko::proc {
struct Process;
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
    proc::Process* process;     // user process this thread runs (nullptr: kernel thread)
    volatile bool kill_pending; // the thread must end before it returns to ring 3 (kill, exit of a sibling)
    bool interrupted;           // its interruptible wait was cancelled by interrupt()
    sync::Semaphore* volatile waiting_sem;  // semaphore of an interruptible wait in progress
    Thread* next;         // run queue / wait queue link
    Thread* all_next;     // list of every thread
    exc::State exc;       // C# try/catch frames and scope guards of this thread
    Priority base_priority;   // `priority` may be raised above it while it holds a mutex others wait for
    uint32_t mutexes_held;
    uint64_t fs_base;     // user programs: their thread-local block (set_tls)
};

using Entry = void (*)(uint64_t arg);

// Adopts the current flow of execution as thread #1 ("kernel") and creates the idle thread.
void init(uint64_t current_stack_top, uint64_t current_stack_size);
bool running();

// Starts a thread; returns its id (0 when out of memory). The thread ends when `entry` returns.
uint32_t spawn(const char* name, Entry entry, uint64_t arg = 0, Priority priority = Priority::Normal);
// Same, for the thread of a user process: it runs with that process's address space.
uint32_t spawn_for(proc::Process* process, const char* name, Entry entry, uint64_t arg);

void yield();
void sleep_ms(uint64_t ms);
NEKO_NORETURN void exit();

Thread* current();
uint32_t current_id();
Priority current_priority();  // effective: raised while it holds a mutex a more urgent thread waits for
const char* current_name();

// Low level, for synchronisation primitives. Interrupts must be disabled by the caller.
// Marks the current thread Blocked and runs something else; `guard` (the primitive's own lock,
// held by the caller) is released only once the scheduler lock is taken, so no wake-up is lost.
void block(const char* reason, sync::RawLock* guard);
void wake(Thread* t);  // Blocked → Ready (callable from interrupt handlers and other CPUs)
// Effective priority (priority inheritance): a Ready thread moves to the right queue.
void set_priority(Thread* t, Priority priority);

// Sets t->kill_pending and cuts short what it is waiting for: a sleep, or an interruptible semaphore
// wait (Semaphore::wait_interruptible returns false). A thread running on another CPU is kicked so it
// notices at once. Callable from interrupt handlers.
void interrupt(Thread* t);
bool killed();  // the current thread has kill_pending set

// Calls fn(t, ctx) for every thread of `process` (scheduler lock held: fn must not block).
void for_each_thread(proc::Process* process, void (*fn)(Thread*, void*), void* ctx);

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
    uint32_t pid;            // user process it runs, 0 for kernel threads
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
    // Like wait(), but gives up (false) when the thread is killed — for waits on behalf of user programs.
    bool wait_interruptible();
    bool try_wait();
    void signal();
    void cancel(sched::Thread* t);  // internal: remove t from the wait queue (sched::interrupt)
    int64_t value() const { return count; }
};

}  // namespace neko::sync
