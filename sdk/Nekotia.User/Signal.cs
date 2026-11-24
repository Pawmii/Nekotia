namespace Nekotia.User;

/// <summary>
/// Signals (runtime/include/neko/process.hpp): asynchronous notifications to a process. By default a signal ends
/// the program with exit code 128 + its number (Ctrl+C = Int → 130, Kill → 137, a broken pipe → 141); SIGCHLD is
/// ignored. A handler runs on the program's own stack the next time it returns from the kernel; a blocking call it
/// was in (read, sleep, wait…) returns <see cref="Process.ErrInterrupted"/>. Kill cannot be caught.
/// </summary>
public static unsafe class Signal
{
    public const int Int = 2, Kill = 9, Usr1 = 10, Usr2 = 12, Pipe = 13, Term = 15, Child = 17;

    /// <summary>Runs <paramref name="handler"/> when <paramref name="sig"/> arrives (null: the default action again).</summary>
    [Native("::neko::user::signal_handle")]
    public static extern long Handle(int sig, delegate*<int, void> handler);

    /// <summary>Drops <paramref name="sig"/> when it arrives (e.g. a shell ignoring Ctrl+C meant for its child).</summary>
    [Native("::neko::user::signal_ignore")]
    public static extern long Ignore(int sig);

    /// <summary>Sends <paramref name="sig"/> to process <paramref name="pid"/>: 0 or ErrNotFound.</summary>
    [Native("::neko::user::signal_send")]
    public static extern long Send(uint pid, int sig);
}
