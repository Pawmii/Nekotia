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
    [Native("threads")] public uint Threads;
}

/// <summary>
/// User programs (runtime/src/core/process.cpp): static ELF64 executables from /bin (the initrd), each run in its
/// own address space in ring 3. Programs are C# too — see user/ and sdk/Nekotia.User.
/// </summary>
public static unsafe class Process
{
    /// <summary>Error results of <see cref="Start"/> and <see cref="Wait"/> (mirrors <c>neko::proc::Error</c>).</summary>
    public const long ErrBadArgument = -1, ErrNotFound = -2, ErrNoMemory = -3, ErrBadAddress = -4,
        ErrNoSyscall = -5, ErrBadExecutable = -6, ErrNotChild = -7;

    public const long ErrInterrupted = -8, ErrLimit = -9;

    /// <summary>File errors (<c>neko/vfs.hpp</c>).</summary>
    public const long ErrExists = -10, ErrNotDirectory = -11, ErrIsDirectory = -12, ErrNotEmpty = -13, ErrBadFd = -14,
        ErrTooManyFiles = -15, ErrNameTooLong = -16, ErrBusy = -17, ErrAccess = -18, ErrNotSupported = -19, ErrIo = -20, ErrBrokenPipe = -21;

    /// <summary>Exit codes: Ctrl+C, kill.</summary>
    public const long ExitInterrupted = 130, ExitKilled = 137, ExitBrokenPipe = 141;

    /// <summary>A process killed by CPU exception <c>v</c> exits with <c>KilledBase + v</c> (142 = page fault).</summary>
    public const long KilledBase = 128;

    /// <summary>
    /// Starts program <paramref name="name"/> (/bin/NAME, or a path when it contains '/') with space-separated
    /// <paramref name="args"/>; returns its pid or a negative error.
    /// </summary>
    [Native("::neko::proc::spawn")]
    public static extern long Start(string name, string args = null, bool detached = false);

    [Native("::neko::proc::spawn")]
    public static extern long Start(byte* name, byte* args, bool detached = false);

    /// <summary>Blocks until process <paramref name="pid"/> exits; returns its exit code (or a negative error).</summary>
    [Native("::neko::proc::wait")]
    public static extern long Wait(uint pid);

    /// <summary>Ends every thread of process <paramref name="pid"/>; it exits with <paramref name="code"/>. 0 or ErrNotFound.</summary>
    [Native("::neko::proc::kill")]
    public static extern long Kill(uint pid, long code = 137);

    /// <summary>Ctrl+C: ends the program in the foreground of the console; false when the kernel shell is.</summary>
    [Native("::neko::proc::interrupt_foreground")]
    public static extern bool InterruptForeground();

    /// <summary>Pid owning the console (whoever the shell is waiting for), 0 = the kernel.</summary>
    [Native("::neko::proc::foreground")]
    public static extern uint Foreground();

    [Native("::neko::proc::count")]
    public static extern uint Count();

    [Native("::neko::proc::info")]
    public static extern bool GetInfo(uint index, out ProcessInfo info);

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
        if (error == ErrInterrupted) return "interrupted";
        if (error == ErrLimit) return "too many threads";
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
        if (error == ErrBrokenPipe) return "broken pipe";
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

    /// <summary>
    /// Copies kernel data into the program's memory — the only allowed way (with SMAP the CPU blocks any other
    /// kernel access to user pages). False on a bad address; never stops the kernel.
    /// </summary>
    [Native("::neko::proc::copy_to_user")]
    public static extern bool CopyTo(void* userDestination, void* source, ulong length);

    [Native("::neko::proc::copy_from_user")]
    public static extern bool CopyFrom(void* destination, void* userSource, ulong length);
}
