// kill [-SIGNAL] PID… — sends a signal (default TERM: the program may clean up; -9 / -KILL cannot be caught).
// SIGNAL is a number or one of INT, KILL, USR1, USR2, PIPE, TERM.
using Nekotia;
using Nekotia.User;

namespace KillProgram;

public static unsafe class Program
{
    public static int Main(int argc, byte** argv)
    {
        if (argc < 2)
        {
            Console.WriteLine("usage: kill [-SIGNAL] PID…   (pids: ps; SIGNAL: number, INT, KILL, USR1, USR2, PIPE, TERM)");
            return 2;
        }
        int first = 1;
        int sig = Signal.Term;
        if (argv[1][0] == '-')
        {
            sig = SignalNumber(argv[1] + 1);
            if (sig <= 0)
            {
                Console.WriteLine("kill: unknown signal");
                return 2;
            }
            first = 2;
        }
        int failed = 0;
        for (int i = first; i < argc; i++)
        {
            long pid = ParsePid(argv[i]);
            if (pid <= 0 || Signal.Send((uint)pid, sig) != 0)
            {
                Console.Write("kill: no process ");
                Console.Write(argv[i]);
                Console.WriteLine();
                failed++;
            }
        }
        return failed == 0 ? 0 : 1;
    }

    static int SignalNumber(byte* s)
    {
        if (Str.Equals(s, "INT")) return Signal.Int;
        if (Str.Equals(s, "KILL")) return Signal.Kill;
        if (Str.Equals(s, "USR1")) return Signal.Usr1;
        if (Str.Equals(s, "USR2")) return Signal.Usr2;
        if (Str.Equals(s, "PIPE")) return Signal.Pipe;
        if (Str.Equals(s, "TERM")) return Signal.Term;
        long n = ParsePid(s);
        return n > 0 && n <= 31 ? (int)n : -1;
    }

    static long ParsePid(byte* s)
    {
        long v = 0;
        if (*s == 0)
            return -1;
        while (*s != 0)
        {
            if (*s < '0' || *s > '9' || v > 100000000)
                return -1;
            v = v * 10 + (*s - '0');
            s++;
        }
        return v;
    }
}
