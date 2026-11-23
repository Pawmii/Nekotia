// SpinLock, Mutex (with deadlock detection) and Semaphore.
#include <neko/runtime.hpp>
#include <neko/sched.hpp>
#include <neko/smp.hpp>

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
    uint32_t me = smp::cpu_index() + 1;
    if (__atomic_load_n(&owner, __ATOMIC_RELAXED) == me) {  // recursion on this CPU
        ++depth;
        return;
    }
    while (__atomic_exchange_n(&locked, 1u, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(&locked, __ATOMIC_RELAXED)) cpu::pause();
    owner = me;
    depth = 1;
    saved_flags = flags;
}

void SpinLock::unlock() {
    if (--depth > 0) return;
    uint64_t flags = saved_flags;
    owner = 0;
    __atomic_store_n(&locked, 0u, __ATOMIC_RELEASE);
    cpu::restore_interrupts(flags);
}

// ---------------------------------------------------------------- Mutex

void Mutex::lock() {
    cpu::InterruptGuard g;
    Thread* me = sched::current();
    if (!sched::running() || !me) return;  // before the scheduler: single flow, nothing to protect
    guard.acquire();
    if (!owner) {
        owner = me;
        ++me->mutexes_held;
        guard.release();
        return;
    }
    if (owner == me) {
        guard.release();
        deadlock_self(this);
    }
    if (cycle(me, this)) {
        guard.release();
        deadlock_cycle(me, this);
    }
    me->waiting_for = this;
    enqueue(head, tail, me);
    // Priority inheritance: whoever holds it (and whoever they wait for, a few links deep) runs at
    // least at our priority until they let go — a low thread must not keep a high one waiting
    // behind medium ones.
    Thread* holder = owner;
    for (int depth = 0; holder && depth < 8; ++depth) {
        if (uint32_t(me->priority) < uint32_t(holder->priority)) sched::set_priority(holder, me->priority);
        Mutex* next = holder->waiting_for;
        holder = next ? next->owner : nullptr;
    }
    sched::block(label ? label : "mutex", &guard);  // releases `guard` under the scheduler lock
    me->waiting_for = nullptr;                      // unlock() handed us the ownership
    ++me->mutexes_held;
}

bool Mutex::try_lock() {
    cpu::InterruptGuard g;
    guard.acquire();
    bool ok = !owner;
    if (ok) {
        owner = sched::current();
        if (owner) ++owner->mutexes_held;
    }
    guard.release();
    return ok;
}

void Mutex::unlock() {
    cpu::InterruptGuard g;
    if (!sched::running()) return;
    guard.acquire();
    if (owner != sched::current()) {
        guard.release();
        crash::fail(crash::StopCode::KernelPanic, "mutex unlocked by a thread that does not own it");
    }
    // The most urgent waiter gets it (first among equals); it inherits the urgency of those still waiting.
    Thread* next = nullptr;
    Thread* before = nullptr;
    for (Thread *x = head, *prev = nullptr; x; prev = x, x = x->next) {
        if (!next || uint32_t(x->priority) < uint32_t(next->priority)) {
            next = x;
            before = prev;
        }
    }
    sched::Priority inherited = sched::Priority::Low;
    if (next) {
        if (before) before->next = next->next;
        else head = next->next;
        if (tail == next) tail = before;
        next->next = nullptr;
        inherited = next->priority;
        for (Thread* x = head; x; x = x->next)
            if (uint32_t(x->priority) < uint32_t(inherited)) inherited = x->priority;
    }
    owner = next;
    Thread* me = sched::current();
    bool restore = --me->mutexes_held == 0 && me->priority != me->base_priority;
    guard.release();
    if (restore) sched::set_priority(me, me->base_priority);
    if (next && uint32_t(inherited) < uint32_t(next->priority)) sched::set_priority(next, inherited);
    if (next) sched::wake(next);
}

// ---------------------------------------------------------------- Semaphore

void Semaphore::wait() {
    cpu::InterruptGuard g;
    guard.acquire();
    if (count > 0) {
        --count;
        guard.release();
        return;
    }
    enqueue(head, tail, sched::current());
    sched::block("semaphore", &guard);
}

bool Semaphore::wait_interruptible() {
    cpu::InterruptGuard g;
    Thread* me = sched::current();
    if (!sched::running() || !me) {
        wait();
        return true;
    }
    guard.acquire();
    if (count > 0) {
        --count;
        guard.release();
        return true;
    }
    // Publish the wait before looking at kill_pending; interrupt() sets the flag before reading
    // waiting_sem. One of the two always sees the other, so a kill is never missed.
    me->interrupted = false;
    __atomic_store_n(&me->waiting_sem, this, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&me->kill_pending, __ATOMIC_SEQ_CST)) {
        me->waiting_sem = nullptr;
        guard.release();
        return false;
    }
    enqueue(head, tail, me);
    sched::block("semaphore", &guard);
    // Woken either by signal() (which handed us a unit) or by cancel().
    __atomic_store_n(&me->waiting_sem, nullptr, __ATOMIC_SEQ_CST);
    return !me->interrupted;
}

void Semaphore::cancel(Thread* t) {
    cpu::InterruptGuard g;
    guard.acquire();
    bool removed = false;
    for (Thread** p = &head; *p; p = &(*p)->next) {
        if (*p == t) {
            *p = t->next;
            if (tail == t) {
                tail = nullptr;
                for (Thread* q = head; q; q = q->next) tail = q;
            }
            t->next = nullptr;
            t->interrupted = true;
            removed = true;
            break;
        }
    }
    guard.release();
    if (removed) sched::wake(t);
}

bool Semaphore::try_wait() {
    cpu::InterruptGuard g;
    guard.acquire();
    bool ok = count > 0;
    if (ok) --count;
    guard.release();
    return ok;
}

void Semaphore::signal() {
    cpu::InterruptGuard g;
    guard.acquire();
    Thread* t = dequeue(head, tail);  // hand the unit straight to a waiter
    if (!t) ++count;
    guard.release();
    if (t) sched::wake(t);
}

}  // namespace neko::sync
