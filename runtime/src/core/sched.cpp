// Preemptive priority round-robin scheduler for several CPUs.
//
// One global lock (g_lock) protects every run queue, the sleep list and thread states.
// It is held *across* the context switch: the outgoing thread takes it, and the incoming
// thread releases it (after_switch). That way no other CPU can pick up a thread whose
// registers are still being saved.
//
// Invariants (interrupts disabled whenever g_lock is held):
//   * a running thread is in no queue; Ready threads are in exactly one run queue
//   * Sleeping threads are in g_sleepers; Blocked ones in the wait queue of a Mutex/Semaphore
//   * each CPU has its own idle thread, which is never queued
#include <neko/process.hpp>
#include <neko/runtime.hpp>
#include <neko/sched.hpp>
#include <neko/smp.hpp>

extern "C" void neko_switch_context(uint64_t* save_rsp, uint64_t new_rsp);
extern "C" void neko_thread_trampoline();

// Platform hooks (arch/x86_64/sched_arch.cpp, host/host_main.cpp).
uint64_t neko_sched_new_stack(size_t size);  // returns the top, 16-byte aligned; 0 when out of memory
void neko_sched_idle();                      // wait for an interrupt (hardware) / for time to pass (host)
void neko_sched_switch_to(neko::sched::Thread* next);  // kernel stack for ring 3 entries, address space

namespace neko::sched {
namespace {

using smp::Cpu;

constexpr uint64_t kSliceMs = 10;
constexpr size_t kStackSize = 32 * 1024;
constexpr size_t kIdleStackSize = 8 * 1024;
constexpr uint32_t kPriorities = 3;
constexpr uint32_t kMaxFreeStacks = 32;

struct Queue {
    Thread* head;
    Thread* tail;
};

sync::RawLock g_lock;
Queue g_ready[kPriorities];
Thread* g_sleepers = nullptr;
Thread* g_all = nullptr;
bool g_running = false;
uint32_t g_next_id = 1;
uint32_t g_count = 0;
uint64_t g_switches = 0;
uint64_t g_free_stacks[kMaxFreeStacks];
uint32_t g_free_stack_count = 0;

alignas(16) uint8_t g_fx_template[512];  // clean FPU/SSE state for new threads

void push(Queue& q, Thread* t) {
    t->next = nullptr;
    if (q.tail) q.tail->next = t;
    else q.head = t;
    q.tail = t;
}

Thread* pop(Queue& q) {
    Thread* t = q.head;
    if (!t) return nullptr;
    q.head = t->next;
    if (!q.head) q.tail = nullptr;
    t->next = nullptr;
    return t;
}

bool any_ready() { return g_ready[0].head || g_ready[1].head || g_ready[2].head; }

// g_lock held.
void make_ready(Thread* t) {
    t->state = State::Ready;
    t->blocked_on = nullptr;
    push(g_ready[uint32_t(t->priority)], t);
    Cpu* me = smp::this_cpu();
    if (me->current && t->priority < me->current->priority) me->need_resched = true;
    // Wake an idle CPU so the thread does not wait for the next timer tick.
    for (uint32_t i = 0; i < smp::kMaxCpus; ++i) {
        Cpu& c = smp::g_cpus[i];
        if (&c != me && c.online && c.current == c.idle) {
            smp::kick(i);
            break;
        }
    }
}

void copy_name(uint8_t* dst, const char* src) {
    uint32_t i = 0;
    for (; src && src[i] && i < 23; ++i) dst[i] = uint8_t(src[i]);
    dst[i] = 0;
}

// Allocates and links a descriptor. Never called with g_lock held.
Thread* new_thread(const char* name, Priority priority) {
    auto* t = static_cast<Thread*>(heap::alloc_zeroed(sizeof(Thread)));
    if (!t) return nullptr;
    copy_name(t->name, name);
    t->priority = priority;
    cpu::InterruptGuard g;
    g_lock.acquire();
    t->id = g_next_id++;
    t->all_next = g_all;
    g_all = t;
    ++g_count;
    g_lock.release();
    return t;
}

void build_frame(Thread* t, uint64_t top, uint64_t entry, uint64_t arg) {
    // Same layout as switch.S: fxsave | rflags | r15 r14 r13 r12 rbx rbp | return address
    auto* frame = reinterpret_cast<uint64_t*>(top - 576);
    mem::copy(frame, g_fx_template, 512);
    frame[64] = 0x2;    // rflags: IF clear — neko_thread_start enables interrupts
    frame[65] = 0;      // r15
    frame[66] = 0;      // r14
    frame[67] = arg;    // r13
    frame[68] = entry;  // r12
    frame[69] = 0;      // rbx
    frame[70] = 0;      // rbp: ends stack traces
    frame[71] = reinterpret_cast<uint64_t>(&neko_thread_trampoline);
    t->rsp = top - 576;
}

// Runs on the incoming thread right after a switch: release the lock, then free whatever
// the previous thread on this CPU left behind.
void after_switch() {
    Cpu* c = smp::this_cpu();
    Thread* z = c->zombie;
    if (z && z != c->current) {
        c->zombie = nullptr;
        if (g_free_stack_count < kMaxFreeStacks && z->stack_size == kStackSize) g_free_stacks[g_free_stack_count++] = z->stack_top;
        for (Thread** p = &g_all; *p; p = &(*p)->all_next) {
            if (*p == z) {
                *p = z->all_next;
                break;
            }
        }
        --g_count;
    } else {
        z = nullptr;
    }
    g_lock.release();
    if (z) {  // outside the scheduler lock
        if (z->process) proc::thread_freed(z);
        heap::free(z);
    }
}

// g_lock held, interrupts off. Returns with g_lock released.
void schedule_locked() {
    Cpu* c = smp::this_cpu();
    c->need_resched = false;
    Thread* prev = c->current;
    Thread* next = nullptr;
    for (uint32_t p = 0; p < kPriorities && !next; ++p) next = pop(g_ready[p]);

    if (!next) {
        if (prev->state == State::Running) {
            g_lock.release();
            return;  // nothing else to run
        }
        next = c->idle;
    }
    if (prev == c->idle) prev->state = State::Ready;
    else if (prev->state == State::Running) make_ready(prev);

    c->slice_left = kSliceMs;
    if (next == prev) {
        prev->state = State::Running;
        g_lock.release();
        return;
    }
    next->state = State::Running;
    next->cpu = c->index;
    ++next->switches;
    ++g_switches;
    c->current = next;
    neko_sched_switch_to(next);
    neko_switch_context(&prev->rsp, next->rsp);
    after_switch();  // possibly on another CPU now
}

[[noreturn]] void idle_loop(uint64_t) {
    for (;;) {
        neko_sched_idle();
        cpu::InterruptGuard g;
        g_lock.acquire();
        if (any_ready()) schedule_locked();
        else g_lock.release();
    }
}

}  // namespace

void init(uint64_t current_stack_top, uint64_t current_stack_size) {
    __asm__ volatile("fxsave %0" : "=m"(g_fx_template));
    reinterpret_cast<uint32_t*>(g_fx_template)[6] = 0x1F80;  // MXCSR: all exceptions masked

    Cpu* c = smp::this_cpu();
    Thread* boot = new_thread("kernel", Priority::Normal);
    boot->state = State::Running;
    boot->stack_top = current_stack_top;
    boot->stack_size = current_stack_size;
    boot->cpu = c->index;
    c->current = boot;

    Thread* idle = new_thread("idle0", Priority::Low);
    uint64_t top = neko_sched_new_stack(kIdleStackSize);
    if (!idle || !top) panic("sched: cannot create the idle thread");
    idle->stack_top = top;
    idle->stack_size = kIdleStackSize;
    idle->state = State::Ready;
    build_frame(idle, top, reinterpret_cast<uint64_t>(&idle_loop), 0);
    c->idle = idle;
    c->slice_left = kSliceMs;
    g_running = true;
}

void cpu_online() {
    Cpu* c = smp::this_cpu();
    char name[8] = {'i', 'd', 'l', 'e', 0, 0, 0, 0};
    if (c->index >= 10) name[4] = char('0' + c->index / 10), name[5] = char('0' + c->index % 10);
    else name[4] = char('0' + c->index);
    Thread* idle = new_thread(name, Priority::Low);
    if (!idle) panic("sched: cannot create an idle thread");
    idle->state = State::Running;
    idle->stack_top = c->stack_top;
    idle->stack_size = kIdleStackSize;
    idle->cpu = c->index;
    c->idle = idle;
    c->current = idle;
    c->slice_left = kSliceMs;
}

void run_idle() { idle_loop(0); }

bool running() { return g_running; }

namespace {
uint32_t start_thread(proc::Process* process, const char* name, Entry entry, uint64_t arg, Priority priority);
}

uint32_t spawn(const char* name, Entry entry, uint64_t arg, Priority priority) {
    return start_thread(nullptr, name, entry, arg, priority);
}

uint32_t spawn_for(proc::Process* process, const char* name, Entry entry, uint64_t arg) {
    return start_thread(process, name, entry, arg, Priority::Normal);
}

namespace {
uint32_t start_thread(proc::Process* process, const char* name, Entry entry, uint64_t arg, Priority priority) {
    if (!g_running || !entry) return 0;
    uint64_t top = 0;
    {
        cpu::InterruptGuard g;
        g_lock.acquire();
        if (g_free_stack_count) top = g_free_stacks[--g_free_stack_count];
        g_lock.release();
    }
    if (!top) top = neko_sched_new_stack(kStackSize);
    if (!top) return 0;
    Thread* t = new_thread(name, priority);
    if (!t) return 0;
    t->stack_top = top;
    t->stack_size = kStackSize;
    t->process = process;
    build_frame(t, top, reinterpret_cast<uint64_t>(entry), arg);
    cpu::InterruptGuard g;
    g_lock.acquire();
    make_ready(t);
    g_lock.release();
    return t->id;
}
}  // namespace

void yield() {
    if (!g_running) return;
    cpu::InterruptGuard g;
    g_lock.acquire();
    schedule_locked();
}

void sleep_ms(uint64_t ms) {
    Cpu* c = smp::this_cpu();
    if (!g_running || c->current == c->idle) {
        uint64_t until = time::uptime_ms() + ms;
        while (time::uptime_ms() < until) neko_sched_idle();
        return;
    }
    cpu::InterruptGuard g;
    g_lock.acquire();
    Thread* t = smp::this_cpu()->current;
    t->state = State::Sleeping;
    t->wake_at = time::uptime_ms() + ms;
    t->blocked_on = "sleep";
    t->next = g_sleepers;
    g_sleepers = t;
    schedule_locked();
}

void exit() {
    cpu::disable_interrupts();
    g_lock.acquire();
    Cpu* c = smp::this_cpu();
    Thread* t = c->current;
    if (t == c->idle) panic("sched: an idle thread exited");
    t->state = State::Dead;
    c->zombie = t;
    schedule_locked();
    __builtin_unreachable();
}

Thread* current() { return smp::this_cpu()->current; }
uint32_t current_id() { return current() ? current()->id : 0; }
const char* current_name() { return current() ? reinterpret_cast<const char*>(current()->name) : "boot"; }

void block(const char* reason, sync::RawLock* guard) {
    g_lock.acquire();
    if (guard) guard->release();
    Thread* t = smp::this_cpu()->current;
    t->state = State::Blocked;
    t->blocked_on = reason;
    schedule_locked();
}

void wake(Thread* t) {
    cpu::InterruptGuard g;
    g_lock.acquire();
    if (t->state == State::Blocked) make_ready(t);
    g_lock.release();
}

void tick() {
    if (!g_running) return;
    Cpu* c = smp::this_cpu();
    if (!c->current) return;  // CPU not fully online yet
    g_lock.acquire();
    if (c->current == c->idle) ++c->idle_ms;
    else ++c->current->cpu_ms;

    if (c->index == 0) {  // one CPU looks after the sleepers
        uint64_t now = time::uptime_ms();
        for (Thread** p = &g_sleepers; *p;) {
            Thread* t = *p;
            if (t->wake_at <= now) {
                *p = t->next;
                make_ready(t);
            } else {
                p = &t->next;
            }
        }
    }
    if (c->slice_left > 0) --c->slice_left;
    if (c->slice_left == 0) c->need_resched = true;
    if (c->current == c->idle && any_ready()) c->need_resched = true;
    g_lock.release();
}

void preempt_if_needed() {
    Cpu* c = smp::this_cpu();
    if (!g_running || !c->need_resched || !c->current) return;
    g_lock.acquire();
    schedule_locked();
}

uint32_t thread_count() { return g_count; }

bool thread_info(uint32_t index, ThreadInfo& out) {
    cpu::InterruptGuard g;
    g_lock.acquire();
    Thread* t = g_all;
    for (uint32_t i = 0; t && i < index; ++i) t = t->all_next;
    if (t) {
        out.id = t->id;
        out.state = t->state;
        out.priority = t->priority;
        mem::copy(out.name, t->name, sizeof(out.name));
        out.cpu_ms = t->cpu_ms;
        for (uint32_t i = 0; i < smp::kMaxCpus; ++i)
            if (smp::g_cpus[i].idle == t) out.cpu_ms = smp::g_cpus[i].idle_ms;
        out.switches = t->switches;
        out.stack_kib = t->stack_size / 1024;
        out.cpu = t->cpu;
        out.blocked_on = t->blocked_on ? t->blocked_on : "";
        out.pid = t->process ? t->process->pid : 0;
    }
    g_lock.release();
    return t != nullptr;
}

uint64_t context_switches() { return g_switches; }

uint64_t idle_ms() {
    uint64_t sum = 0;
    for (uint32_t i = 0; i < smp::kMaxCpus; ++i) sum += smp::g_cpus[i].idle_ms;
    return sum;
}

bool has_sleepers() { return g_sleepers != nullptr; }

void thread_started() { after_switch(); }

}  // namespace neko::sched

// Entered from neko_thread_trampoline on a fresh stack, with g_lock held by the switch.
extern "C" [[noreturn]] void neko_thread_start(neko::sched::Entry entry, uint64_t arg) {
    neko::sched::thread_started();
    neko::cpu::enable_interrupts();
    entry(arg);
    neko::sched::exit();
}
