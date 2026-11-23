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
}

/// <summary>The running program and the processes it starts (system calls, see runtime/include/neko/process.hpp).</summary>
public static unsafe class Process
{
    /// <summary>Negative results of Start/Wait.</summary>
    public const long ErrBadArgument = -1, ErrNotFound = -2, ErrNoMemory = -3, ErrBadAddress = -4,
        ErrNoSyscall = -5, ErrBadExecutable = -6, ErrNotChild = -7;

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

    /// <summary>Starts program <paramref name="name"/> from /boot/bin; returns its pid or a negative error.</summary>
    [Native("::neko::user::spawn")]
    public static extern long Start(string name, string args = null);

    [Native("::neko::user::spawn")]
    public static extern long Start(byte* name, byte* args);

    /// <summary>Waits for a child to exit and returns its exit code (or a negative error).</summary>
    [Native("::neko::user::wait")]
    public static extern long Wait(uint pid);

    /// <summary>Process number <paramref name="index"/> in the kernel's table; false past the end.</summary>
    [Native("::neko::user::process_info")]
    public static extern bool GetInfo(uint index, out ProcessInfo info);

    /// <summary>Command-line arguments; Arg(0) is the program name.</summary>
    [Native("::neko::user::argc")]
    public static extern int ArgCount();

    [Native("::neko::user::arg")]
    public static extern byte* Arg(int index);

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
