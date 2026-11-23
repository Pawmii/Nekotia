namespace Nekotia.Processes;

/// <summary>Mirrors <c>neko::proc::State</c>.</summary>
[Native("::neko::proc::State")]
public enum ProcessState : byte { Running = 0, Exited = 1 }

/// <summary>Snapshot of one user process. Mirrors <c>neko::proc::ProcessInfo</c>.</summary>
[Native("::neko::proc::ProcessInfo")]
public unsafe struct ProcessInfo
{
    [Native("pid")] public uint Pid;
    /// <summary>Pid of the parent process; 0 when the kernel started it.</summary>
    [Native("parent")] public uint Parent;
    [Native("thread_id")] public uint ThreadId;
    [Native("state")] public ProcessState State;
    [Native("name")] public fixed byte Name[24];
    /// <summary>User memory: image, stack and allocations.</summary>
    [Native("memory_kib")] public ulong MemoryKib;
    [Native("uptime_ms")] public ulong UptimeMs;
}

/// <summary>
/// User programs (runtime/src/core/process.cpp): static ELF64 executables from /boot/bin, each run in its own
/// address space in ring 3 by one thread. Programs are C# too — see user/ and sdk/Nekotia.User.
/// </summary>
public static unsafe class Process
{
    /// <summary>Error results of <see cref="Start"/> and <see cref="Wait"/> (mirrors <c>neko::proc::Error</c>).</summary>
    public const long ErrBadArgument = -1, ErrNotFound = -2, ErrNoMemory = -3, ErrBadAddress = -4,
        ErrNoSyscall = -5, ErrBadExecutable = -6, ErrNotChild = -7;

    /// <summary>A process killed by CPU exception <c>v</c> exits with <c>KilledBase + v</c> (142 = page fault).</summary>
    public const long KilledBase = 128;

    /// <summary>Starts program <paramref name="name"/> with space-separated <paramref name="args"/>; returns its pid or a negative error.</summary>
    [Native("::neko::proc::spawn")]
    public static extern long Start(string name, string args = null, bool detached = false);

    [Native("::neko::proc::spawn")]
    public static extern long Start(byte* name, byte* args, bool detached = false);

    /// <summary>Blocks until process <paramref name="pid"/> exits; returns its exit code (or a negative error).</summary>
    [Native("::neko::proc::wait")]
    public static extern long Wait(uint pid);

    [Native("::neko::proc::count")]
    public static extern uint Count();

    [Native("::neko::proc::info")]
    public static extern bool GetInfo(uint index, out ProcessInfo info);

    /// <summary>Programs found on the boot medium.</summary>
    [Native("::neko::proc::module_count")]
    public static extern uint ProgramCount();

    [Native("::neko::proc::module_name")]
    public static extern string ProgramName(uint index);

    [Native("::neko::proc::module_size")]
    public static extern ulong ProgramSize(uint index);

    /// <summary>Ends the calling user process (from a [Syscall] handler, which runs on the process's thread).</summary>
    [Native("::neko::proc::exit_current")]
    [NoReturn]
    public static extern void ExitCurrent(long code);

    /// <summary>Pid of the process the calling thread belongs to; 0 in kernel threads.</summary>
    public static uint CurrentPid() => CurrentPidNative();

    [Native("::neko::proc::current_pid")]
    static extern uint CurrentPidNative();

    /// <summary>Whether a program killed by a CPU exception is reported on the terminal (default: yes).</summary>
    [Native("::neko::proc::set_fault_reports")]
    public static extern void SetFaultReports(bool on);

    /// <summary>Human-readable text for a negative result of Start/Wait.</summary>
    public static string ErrorText(long error)
    {
        if (error == ErrBadArgument) return "bad argument";
        if (error == ErrNotFound) return "not found";
        if (error == ErrNoMemory) return "out of memory";
        if (error == ErrBadAddress) return "bad address";
        if (error == ErrNoSyscall) return "user mode is not available";
        if (error == ErrBadExecutable) return "not a valid static x86_64 ELF executable";
        if (error == ErrNotChild) return "not a child of the caller";
        return "unknown error";
    }
}

/// <summary>Checks for pointers that come from a user program (in [Syscall] handlers).</summary>
public static unsafe class UserMemory
{
    /// <summary>True when the calling process may read <paramref name="length"/> bytes at <paramref name="address"/>.</summary>
    [Native("::neko::proc::check_read")]
    public static extern bool CanRead(void* address, ulong length);

    [Native("::neko::proc::check_write")]
    public static extern bool CanWrite(void* address, ulong length);
}
