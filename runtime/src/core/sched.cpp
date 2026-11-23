// Preemptive priority round-robin scheduler (single CPU for now; SMP in ROADMAP phase 4).
//
// Invariants (all scheduler state is touched with interrupts disabled):
//   * the running thread is never in a run queue
//   * Ready threads are in exactly one run queue (by priority); Sleeping ones in g_sleepers
//   * Blocked threads are in some wait queue owned by a Mutex/Semaphore
//   * the idle thread is never queued: it runs only when every queue is empty
#include <neko/runtime.hpp>
#include <neko/sched.hpp>

extern "C" void neko_switch_context(uint64_t* save_rsp, uint64_t new_rsp);
extern "C" void neko_thread_trampoline();

// Platform hooks (arch/x86_64/sched_arch.cpp, host/host_main.cpp).
uint64_t neko_sched_new_stack(size_t size);  // returns the top, 16-byte aligned; 0 when out of memory
void neko_sched_idle();                      // wait for an interrupt (hardware) / for time to pass (host)

namespace neko::sched {
namespace {

constexpr uint64_t kSliceMs = 10;
constexpr size_t kStackSize = 32 * 1024;
constexpr uint32_t kPriorities = 3;
constexpr uint32_t kMaxFreeStacks = 32;

struct Queue {
    Thread* head;
    Thread* tail;
};

Queue g_ready[kPriorities];
Thread* g_sleepers = nullptr;
Thread* g_all = nullptr;
Thread* g_current = nullptr;
Thread* g_idle = nullptr;
Thread* g_zombie = nullptr;  // exited thread whose stack is still in use until the next switch
bool g_running = false;
bool g_need_resched = false;
uint32_t g_next_id = 1;
uint32_t g_count = 0;
uint64_t g_slice_left = kSliceMs;
uint64_t g_switches = 0;
uint64_t g_idle_ms = 0;
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

void make_ready(Thread* t) {
    t->state = State::Ready;
    t->blocked_on = nullptr;
    push(g_ready[uint32_t(t->priority)], t);
    // A more important thread became runnable: switch as soon as possible.
    if (g_current && t->priority < g_current->priority) g_need_resched = true;
}

void copy_name(uint8_t* dst, const char* src) {
    uint32_t i = 0;
    for (; src && src[i] && i < 23; ++i) dst[i] = uint8_t(src[i]);
    dst[i] = 0;
}

Thread* new_thread(const char* name, Priority priority) {
    auto* t = static_cast<Thread*>(heap::alloc_zeroed(sizeof(Thread)));
    if (!t) return nullptr;
    t->id = g_next_id++;
    t->priority = priority;
    copy_name(t->name, name);
    t->all_next = g_all;
    g_all = t;
    ++g_count;
    return t;
}

uint64_t take_stack() {
    if (g_free_stack_count) return g_free_stacks[--g_free_stack_count];
    return neko_sched_new_stack(kStackSize);
}

void reap_zombie() {
    Thread* z = g_zombie;
    if (!z || z == g_current) return;
    g_zombie = nullptr;
    if (g_free_stack_count < kMaxFreeStacks && z->stack_size == kStackSize) g_free_stacks[g_free_stack_count++] = z->stack_top;
    // unlink from the list of all threads and free the descriptor
    for (Thread** p = &g_all; *p; p = &(*p)->all_next) {
        if (*p == z) {
            *p = z->all_next;
            break;
        }
    }
    --g_count;
    heap::free(z);
}

// Picks the next thread and switches to it. Interrupts must be off.
void schedule() {
    g_need_resched = false;
    Thread* prev = g_current;
    Thread* next = nullptr;
    for (uint32_t p = 0; p < kPriorities && !next; ++p) next = pop(g_ready[p]);

    if (!next) {
        if (prev->state == State::Running) return;  // nothing else to run: keep going
        next = g_idle;
    }
    if (prev == g_idle) prev->state = State::Ready;  // idle is never queued
    else if (prev->state == State::Running) make_ready(prev);

    g_slice_left = kSliceMs;
    if (next == prev) {
        prev->state = State::Running;
        return;
    }
    next->state = State::Running;
    ++next->switches;
    ++g_switches;
    g_current = next;
    neko_switch_context(&prev->rsp, next->rsp);
    // Back on `prev`'s stack: a thread that exited before us can now be freed.
    reap_zombie();
}

[[noreturn]] void idle_loop(uint64_t) {
    for (;;) {
        neko_sched_idle();
        cpu::InterruptGuard g;
        if (g_ready[0].head || g_ready[1].head || g_ready[2].head) schedule();
    }
}

}  // namespace

void init(uint64_t current_stack_top, uint64_t current_stack_size) {
    __asm__ volatile("fxsave %0" : "=m"(g_fx_template));
    reinterpret_cast<uint32_t*>(g_fx_template)[6] = 0x1F80;  // MXCSR: all exceptions masked

    Thread* boot = new_thread("kernel", Priority::Normal);
    boot->state = State::Running;
    boot->stack_top = current_stack_top;
    boot->stack_size = current_stack_size;
    g_current = boot;

    g_idle = new_thread("idle", Priority::Low);
    uint64_t top = neko_sched_new_stack(8 * 1024);
    if (!g_idle || !top) panic("sched: cannot create the idle thread");
    g_idle->stack_top = top;
    g_idle->stack_size = 8 * 1024;
    // Hand-built switch frame (see switch.S): fxsave | rflags | r15 r14 r13 r12 rbx rbp | ret
    auto* frame = reinterpret_cast<uint64_t*>(top - 576);
    mem::copy(frame, g_fx_template, 512);
    frame[64] = 0x2;                                                    // rflags (IF set by the trampoline)
    frame[65] = 0; frame[66] = 0; frame[67] = 0;                        // r15 r14 r13
    frame[68] = reinterpret_cast<uint64_t>(&idle_loop);                 // r12 = entry
    frame[69] = 0; frame[70] = 0;                                       // rbx rbp
    frame[71] = reinterpret_cast<uint64_t>(&neko_thread_trampoline);    // return address
    g_idle->rsp = top - 576;
    g_idle->state = State::Ready;
    g_running = true;
}

bool running() { return g_running; }

uint32_t spawn(const char* name, Entry entry, uint64_t arg, Priority priority) {
    if (!g_running || !entry) return 0;
    uint64_t top = take_stack();
    if (!top) return 0;
    cpu::InterruptGuard g;
    Thread* t = new_thread(name, priority);
    if (!t) return 0;
    t->stack_top = top;
    t->stack_size = kStackSize;

    auto* frame = reinterpret_cast<uint64_t*>(top - 576);
    mem::copy(frame, g_fx_template, 512);
    frame[64] = 0x2;
    frame[65] = 0;                                   // r15
    frame[66] = 0;                                   // r14
    frame[67] = arg;                                 // r13 = argument
    frame[68] = reinterpret_cast<uint64_t>(entry);   // r12 = entry
    frame[69] = 0;                                   // rbx
    frame[70] = 0;                                   // rbp: ends stack traces
    frame[71] = reinterpret_cast<uint64_t>(&neko_thread_trampoline);
    t->rsp = top - 576;
    make_ready(t);
    return t->id;
}

void yield() {
    if (!g_running) return;
    cpu::InterruptGuard g;
    schedule();
}

void sleep_ms(uint64_t ms) {
    if (!g_running || g_current == g_idle) {
        uint64_t until = time::uptime_ms() + ms;
        while (time::uptime_ms() < until) neko_sched_idle();
        return;
    }
    cpu::InterruptGuard g;
    Thread* t = g_current;
    t->state = State::Sleeping;
    t->wake_at = time::uptime_ms() + ms;
    t->blocked_on = "sleep";
    t->next = g_sleepers;
    g_sleepers = t;
    schedule();
}

void exit() {
    cpu::disable_interrupts();
    Thread* t = g_current;
    if (t == g_idle) panic("sched: the idle thread exited");
    reap_zombie();
    t->state = State::Dead;
    g_zombie = t;
    schedule();
    __builtin_unreachable();
}

Thread* current() { return g_current; }
uint32_t current_id() { return g_current ? g_current->id : 0; }
const char* current_name() { return g_current ? reinterpret_cast<const char*>(g_current->name) : "boot"; }

void block(const char* reason) {
    Thread* t = g_current;
    t->state = State::Blocked;
    t->blocked_on = reason;
    schedule();
}

void wake(Thread* t) {
    if (t->state == State::Blocked || t->state == State::Sleeping) make_ready(t);
}

void tick() {
    if (!g_running) return;
    if (g_current == g_idle) ++g_idle_ms;
    else ++g_current->cpu_ms;

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
    if (g_slice_left > 0) --g_slice_left;
    if (g_slice_left == 0) g_need_resched = true;
    if (g_current == g_idle && (g_ready[0].head || g_ready[1].head || g_ready[2].head)) g_need_resched = true;
}

void preempt_if_needed() {
    if (g_running && g_need_resched) schedule();
}

uint32_t thread_count() { return g_count; }

bool thread_info(uint32_t index, ThreadInfo& out) {
    cpu::InterruptGuard g;
    Thread* t = g_all;
    for (uint32_t i = 0; t && i < index; ++i) t = t->all_next;
    if (!t) return false;
    out.id = t->id;
    out.state = t->state;
    out.priority = t->priority;
    mem::copy(out.name, t->name, sizeof(out.name));
    out.cpu_ms = t == g_idle ? g_idle_ms : t->cpu_ms;
    out.switches = t->switches;
    out.stack_kib = t->stack_size / 1024;
    out.blocked_on = t->blocked_on ? t->blocked_on : "";
    return true;
}

uint64_t context_switches() { return g_switches; }
uint64_t idle_ms() { return g_idle_ms; }
bool has_sleepers() { return g_sleepers != nullptr; }

}  // namespace neko::sched

// Entered from neko_thread_trampoline on a fresh stack.
extern "C" [[noreturn]] void neko_thread_start(neko::sched::Entry entry, uint64_t arg) {
    neko::cpu::enable_interrupts();
    entry(arg);
    neko::sched::exit();
}
