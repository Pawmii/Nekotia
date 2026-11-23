namespace Nekotia.User;

/// <summary>Mirrors <c>neko::user::ProcessInfo</c> (the kernel's <c>neko::proc::ProcessInfo</c>).</summary>
[Native("::neko::user::ProcessInfo")]
public unsafe struct ProcessInfo
{
    [Native("pid")] public uint Pid;
    /// <summary>0 when the kernel started it.</summary>
    [Native("parent")] public uint Parent;
    [Native("thread_id")] public uint ThreadId;
    /// <summary>0 running, 1 exited (waiting for its parent).</summary>
    [Native("state")] public byte State;
    [Native("name")] public fixed byte Name[24];
    [Native("memory_kib")] public ulong MemoryKib;
    [Native("uptime_ms")] public ulong UptimeMs;
    [Native("threads")] public uint Threads;
}

/// <summary>The running program and the processes it starts (system calls, see runtime/include/neko/process.hpp).</summary>
public static unsafe class Process
{
    /// <summary>Negative results of Start/Wait.</summary>
    public const long ErrBadArgument = -1, ErrNotFound = -2, ErrNoMemory = -3, ErrBadAddress = -4,
        ErrNoSyscall = -5, ErrBadExecutable = -6, ErrNotChild = -7, ErrInterrupted = -8, ErrLimit = -9;

    /// <summary>File errors.</summary>
    public const long ErrExists = -10, ErrNotDirectory = -11, ErrIsDirectory = -12, ErrNotEmpty = -13, ErrBadFd = -14,
        ErrTooManyFiles = -15, ErrNameTooLong = -16, ErrBusy = -17, ErrAccess = -18, ErrNotSupported = -19, ErrIo = -20;

    /// <summary>Exit codes set by the kernel: Ctrl+C, kill; a CPU exception gives 128 + vector (142 = page fault).</summary>
    public const long ExitInterrupted = 130, ExitKilled = 137;

    /// <summary>Ends the program with <paramref name="code"/> (what Main returning does too).</summary>
    [Native("::neko::user::exit")]
    [NoReturn]
    public static extern void Exit(int code);

    [Native("::neko::user::pid")]
    public static extern uint Id();

    [Native("::neko::user::yield")]
    public static extern void Yield();

    [Native("::neko::user::sleep_ms")]
    public static extern void Sleep(ulong ms);

    /// <summary>
    /// Starts program <paramref name="name"/> (/bin/NAME, or a path when it contains '/'); returns its pid or a
    /// negative error. The child inherits fds 0–2 and the working directory.
    /// </summary>
    [Native("::neko::user::spawn")]
    public static extern long Start(string name, string args = null);

    [Native("::neko::user::spawn")]
    public static extern long Start(byte* name, byte* args);

    /// <summary>Waits for a child to exit and returns its exit code (or a negative error).</summary>
    [Native("::neko::user::wait")]
    public static extern long Wait(uint pid);

    /// <summary>Ends every thread of process <paramref name="pid"/> (exit code 137). 0 or ErrNotFound.</summary>
    [Native("::neko::user::kill")]
    public static extern long Kill(uint pid);

    /// <summary>Process number <paramref name="index"/> in the kernel's table; false past the end.</summary>
    [Native("::neko::user::process_info")]
    public static extern bool GetInfo(uint index, out ProcessInfo info);

    /// <summary>Command-line arguments; Arg(0) is the program name.</summary>
    [Native("::neko::user::argc")]
    public static extern int ArgCount();

    [Native("::neko::user::arg")]
    public static extern byte* Arg(int index);

    /// <summary>Human-readable text for a negative result.</summary>
    public static string ErrorText(long error)
    {
        if (error == ErrBadArgument) return "bad argument";
        if (error == ErrNotFound) return "not found";
        if (error == ErrNoMemory) return "out of memory";
        if (error == ErrBadAddress) return "bad address";
        if (error == ErrBadExecutable) return "not an executable";
        if (error == ErrNotChild) return "not a child";
        if (error == ErrInterrupted) return "interrupted";
        if (error == ErrLimit) return "limit reached";
        if (error == ErrExists) return "already exists";
        if (error == ErrNotDirectory) return "not a directory";
        if (error == ErrIsDirectory) return "is a directory";
        if (error == ErrNotEmpty) return "directory not empty";
        if (error == ErrBadFd) return "bad file descriptor";
        if (error == ErrTooManyFiles) return "too many open files";
        if (error == ErrNameTooLong) return "name too long";
        if (error == ErrBusy) return "busy";
        if (error == ErrAccess) return "access denied";
        if (error == ErrNotSupported) return "not supported";
        if (error == ErrIo) return "I/O error";
        return "error";
    }

    /// <summary>A system call the kernel implements in C# with [Syscall(n)].</summary>
    [Native("::neko::user::syscall")]
    public static extern long Syscall(ulong number, ulong a = 0, ulong b = 0, ulong c = 0);
}

public static class Time
{
    [Native("::neko::user::uptime_ms")]
    public static extern ulong UptimeMs();
}

/// <summary>The program's heap: first fit over pages the kernel maps on demand (the alloc system call).</summary>
public static unsafe class Memory
{
    [Native("::neko::user::alloc")]
    public static extern void* Alloc(ulong size);

    [Native("::neko::user::alloc_zeroed")]
    public static extern void* AllocZeroed(ulong size);

    [Native("::neko::user::free")]
    public static extern void Free(void* block);

    [Native("::neko::user::heap_used")]
    public static extern ulong UsedBytes();

    [Native("::neko::user::heap_total")]
    public static extern ulong TotalBytes();

    public static T* New<T>() where T : unmanaged => (T*)AllocZeroed((ulong)sizeof(T));

    public static T* NewArray<T>(ulong count) where T : unmanaged => (T*)AllocZeroed(count * (ulong)sizeof(T));

    public static void Delete<T>(T* value) where T : unmanaged => Free(value);
}

/// <summary>
/// Threads of this program: they share its memory and run in parallel on every CPU. The program ends when Main
/// returns or any thread calls Process.Exit; a fault in any thread ends all of them.
/// </summary>
public static unsafe class Thread
{
    /// <summary>Runs <paramref name="entry"/>(<paramref name="arg"/>) in a new thread with a 64 KiB stack; returns a handle for Join (0 on failure).</summary>
    [Native("::neko::user::thread_start")]
    public static extern ulong Start(delegate*<ulong, void> entry, ulong arg = 0);

    /// <summary>Waits until the thread's entry method has returned and frees its stack.</summary>
    [Native("::neko::user::thread_join")]
    public static extern void Join(ulong handle);

    /// <summary>Kernel id of the calling thread.</summary>
    [Native("::neko::user::thread_id")]
    public static extern uint CurrentId();

    [Native("::neko::user::thread_exit")]
    [NoReturn]
    public static extern void Exit();

    [Native("::neko::user::yield")]
    public static extern void Yield();
}

/// <summary>Spin lock that yields while it waits. Keep it in a static field; in .neko files: <c>lock (_l) { }</c>.</summary>
[Native("::neko::user::Lock")]
public struct SpinLock
{
    [Native("lock")] public extern void Lock();
    [Native("unlock")] public extern void Unlock();
}

/// <summary>Sequentially consistent atomic operations on 64-bit integers.</summary>
public static unsafe class Atomic
{
    /// <summary>Adds and returns the new value.</summary>
    [Native("::neko::user::atomic_add")]
    public static extern long Add(long* location, long value);

    [Native("::neko::user::atomic_load")]
    public static extern long Load(long* location);

    [Native("::neko::user::atomic_store")]
    public static extern void Store(long* location, long value);

    /// <summary>Sets *location to <paramref name="desired"/> if it equals <paramref name="expected"/>; true on success.</summary>
    [Native("::neko::user::atomic_cas")]
    public static extern bool CompareExchange(long* location, long expected, long desired);
}
