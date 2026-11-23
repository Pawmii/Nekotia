// SpinLock, Mutex (with deadlock detection) and Semaphore.
#include <neko/runtime.hpp>
#include <neko/sched.hpp>

namespace neko::sync {
namespace {

using sched::Thread;

void enqueue(Thread*& head, Thread*& tail, Thread* t) {
    t->next = nullptr;
    if (tail) tail->next = t;
    else head = t;
    tail = t;
}

Thread* dequeue(Thread*& head, Thread*& tail) {
    Thread* t = head;
    if (!t) return nullptr;
    head = t->next;
    if (!head) tail = nullptr;
    t->next = nullptr;
    return t;
}

// "kernel" (#1) — used in deadlock reports
struct Text {
    char buf[480];
    size_t len = 0;
    void add(const char* s) {
        while (s && *s && len + 1 < sizeof(buf)) buf[len++] = *s++;
        buf[len] = 0;
    }
    void add(const uint8_t* s) { add(reinterpret_cast<const char*>(s)); }
    void add_num(uint64_t v) {
        char tmp[21];
        int i = 0;
        do tmp[i++] = char('0' + v % 10), v /= 10;
        while (v);
        while (i) {
            char c[2] = {tmp[--i], 0};
            add(c);
        }
    }
    void thread(const Thread* t) {
        add("'");
        add(t->name);
        add("' (#");
        add_num(t->id);
        add(")");
    }
    void mutex(const Mutex* m) {
        add("mutex '");
        add(m->label ? m->label : "?");
        add("'");
    }
};

Text g_report;

[[noreturn]] void deadlock_self(Mutex* m) {
    g_report.len = 0;
    g_report.add("thread ");
    g_report.thread(sched::current());
    g_report.add(" locks ");
    g_report.mutex(m);
    g_report.add(" that it already holds (mutexes are not recursive)");
    crash::fail(crash::StopCode::Deadlock, g_report.buf);
}

// Follows owner → mutex it waits for → owner … starting at `m`; true when it comes back to `me`.
bool cycle(Thread* me, Mutex* m) {
    Mutex* cur = m;
    for (int depth = 0; cur && cur->owner && depth < 64; ++depth) {
        if (cur->owner == me) return true;
        cur = cur->owner->waiting_for;
    }
    return false;
}

[[noreturn]] void deadlock_cycle(Thread* me, Mutex* m) {
    g_report.len = 0;
    g_report.thread(me);
    g_report.add(" waits for ");
    Mutex* cur = m;
    for (int depth = 0; cur && depth < 8; ++depth) {
        g_report.mutex(cur);
        g_report.add(" held by ");
        g_report.thread(cur->owner);
        if (cur->owner == me) break;
        g_report.add(", which waits for ");
        cur = cur->owner->waiting_for;
    }
    crash::fail(crash::StopCode::Deadlock, g_report.buf);
}

}  // namespace

// ---------------------------------------------------------------- SpinLock

void SpinLock::lock() {
    uint64_t flags = cpu::save_and_disable_interrupts();
    if (locked && depth > 0) {  // recursion on this CPU (single CPU: whoever holds it is us)
        ++depth;
        return;
    }
    while (__atomic_exchange_n(&locked, 1u, __ATOMIC_ACQUIRE)) cpu::pause();
    depth = 1;
    saved_flags = flags;
}

void SpinLock::unlock() {
    if (--depth > 0) return;
    uint64_t flags = saved_flags;
    __atomic_store_n(&locked, 0u, __ATOMIC_RELEASE);
    cpu::restore_interrupts(flags);
}

// ---------------------------------------------------------------- Mutex

void Mutex::lock() {
    cpu::InterruptGuard g;
    Thread* me = sched::current();
    if (!sched::running() || !me) return;  // before the scheduler: single flow, nothing to protect
    if (!owner) {
        owner = me;
        return;
    }
    if (owner == me) deadlock_self(this);
    if (cycle(me, this)) deadlock_cycle(me, this);
    me->waiting_for = this;
    enqueue(head, tail, me);
    sched::block(label ? label : "mutex");
    me->waiting_for = nullptr;  // unlock() handed us the ownership
}

bool Mutex::try_lock() {
    cpu::InterruptGuard g;
    if (owner) return false;
    owner = sched::current();
    return true;
}

void Mutex::unlock() {
    cpu::InterruptGuard g;
    if (!sched::running()) return;
    if (owner != sched::current()) crash::fail(crash::StopCode::KernelPanic, "mutex unlocked by a thread that does not own it");
    owner = dequeue(head, tail);
    if (owner) sched::wake(owner);
}

// ---------------------------------------------------------------- Semaphore

void Semaphore::wait() {
    cpu::InterruptGuard g;
    if (count > 0) {
        --count;
        return;
    }
    Thread* me = sched::current();
    enqueue(head, tail, me);
    sched::block("semaphore");
}

bool Semaphore::try_wait() {
    cpu::InterruptGuard g;
    if (count <= 0) return false;
    --count;
    return true;
}

void Semaphore::signal() {
    cpu::InterruptGuard g;
    if (Thread* t = dequeue(head, tail)) sched::wake(t);  // hand the unit straight to a waiter
    else ++count;
}

}  // namespace neko::sync
