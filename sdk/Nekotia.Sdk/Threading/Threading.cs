namespace Nekotia.Threading;

/// <summary>Mirrors <c>neko::sched::Priority</c>. Higher priorities always run first; equal ones share the CPU in 10 ms slices.</summary>
[Native("::neko::sched::Priority")]
public enum ThreadPriority : byte { High = 0, Normal = 1, Low = 2 }

/// <summary>Mirrors <c>neko::sched::State</c>.</summary>
[Native("::neko::sched::State")]
public enum ThreadState : byte { Ready = 0, Running = 1, Sleeping = 2, Blocked = 3, Dead = 4 }

/// <summary>Snapshot of one thread (for ps). Mirrors <c>neko::sched::ThreadInfo</c>.</summary>
[Native("::neko::sched::ThreadInfo")]
public unsafe struct ThreadInfo
{
    [Native("id")] public uint Id;
    [Native("state")] public ThreadState State;
    [Native("priority")] public ThreadPriority Priority;
    [Native("name")] public fixed byte Name[24];
    [Native("cpu_ms")] public ulong CpuMs;
    [Native("switches")] public ulong Switches;
    [Native("stack_kib")] public ulong StackKib;
    /// <summary>CPU the thread last ran on.</summary>
    [Native("cpu")] public uint Cpu;
    /// <summary>What a blocked thread waits for ("sleep", "semaphore", a mutex name), or "".</summary>
    [Native("blocked_on")] public string BlockedOn;
    /// <summary>User process the thread runs; 0 for kernel threads.</summary>
    [Native("pid")] public uint Pid;
}

/// <summary>
/// Kernel threads (runtime/src/core/sched.cpp): preemptive, priority round-robin, each on its own
/// 32 KiB stack with a guard page. A thread ends when its entry method returns.
/// </summary>
public static unsafe class Thread
{
    /// <summary>Starts <paramref name="entry"/>(<paramref name="arg"/>) in a new thread; returns its id (0 on failure).</summary>
    [Native("::neko::sched::spawn")]
    public static extern uint Start(string name, delegate*<ulong, void> entry, ulong arg = 0,
        ThreadPriority priority = ThreadPriority.Normal);

    /// <summary>Gives the CPU to another ready thread of the same or higher priority.</summary>
    [Native("::neko::sched::yield")]
    public static extern void Yield();

    /// <summary>Blocks the current thread for at least <paramref name="ms"/> milliseconds.</summary>
    [Native("::neko::sched::sleep_ms")]
    public static extern void Sleep(ulong ms);

    [Native("::neko::sched::exit")]
    [NoReturn]
    public static extern void Exit();

    /// <summary>The calling thread is being killed (its program exits): long waits should give up.</summary>
    [Native("::neko::sched::killed")]
    public static extern bool Killed();

    [Native("::neko::sched::current_id")]
    public static extern uint CurrentId();

    /// <summary>The caller's effective priority (raised while it holds a mutex a more urgent thread waits for).</summary>
    [Native("::neko::sched::current_priority")]
    public static extern ThreadPriority CurrentPriority();

    /// <summary>Index of the CPU executing the caller (0 = bootstrap processor).</summary>
    [Native("::neko::smp::cpu_index")]
    public static extern uint CurrentCpu();

    [Native("::neko::sched::current_name")]
    public static extern string CurrentName();

    [Native("::neko::sched::thread_count")]
    public static extern uint Count();

    /// <summary>Thread number <paramref name="index"/> (0 … Count()-1); false past the end.</summary>
    [Native("::neko::sched::thread_info")]
    public static extern bool GetInfo(uint index, out ThreadInfo info);

    [Native("::neko::sched::context_switches")]
    public static extern ulong ContextSwitches();

    [Native("::neko::sched::idle_ms")]
    public static extern ulong IdleMs();
}

/// <summary>
/// Sleeping lock. Not recursive: locking it twice from the same thread, or threads waiting for each
/// other's mutexes in a cycle, stops the kernel with a DEADLOCK report naming the threads and mutexes.
/// In .neko files: <c>lock (_mutex) { ... }</c>. Must live in a static field (never copy a Mutex).
/// </summary>
[Native("::neko::sync::Mutex")]
public struct Mutex
{
    [Native("lock")] public extern void Lock();
    [Native("try_lock")] public extern bool TryLock();
    [Native("unlock")] public extern void Unlock();
    /// <summary>Name shown in deadlock reports and ps.</summary>
    [Native("set_name")] public extern void SetName(string name);
    [Native("is_locked")] public extern bool IsLocked();
}

/// <summary>Counting semaphore; <see cref="Signal"/> may be called from interrupt handlers.</summary>
[Native("::neko::sync::Semaphore")]
public struct Semaphore
{
    [Native("wait")] public extern void Wait();
    /// <summary>Like Wait, but returns false when the calling thread is being killed (waits on behalf of user programs).</summary>
    [Native("wait_interruptible")] public extern bool WaitInterruptible();
    [Native("try_wait")] public extern bool TryWait();
    /// <summary>Waits at most <paramref name="timeoutMs"/>: 1 got it, 0 timed out, -1 the thread is being killed.</summary>
    [Native("wait_for")] public extern int WaitFor(ulong timeoutMs, bool interruptible = true);
    [Native("signal")] public extern void Signal();
    [Native("value")] public extern long Value();
}

/// <summary>Busy-waiting lock that also disables interrupts — for very short sections shared with IRQ handlers.</summary>
[Native("::neko::sync::SpinLock")]
public struct SpinLock
{
    [Native("lock")] public extern void Lock();
    [Native("unlock")] public extern void Unlock();
}
