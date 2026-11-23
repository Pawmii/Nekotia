// kill PID… — ends processes (exit code 137).
using Nekotia;
using Nekotia.User;

namespace KillProgram;

public static unsafe class Program
{
    public static int Main(int argc, byte** argv)
    {
        if (argc < 2)
        {
            Console.WriteLine("usage: kill PID…   (pids: ps)");
            return 2;
        }
        int failed = 0;
        for (int i = 1; i < argc; i++)
        {
            long pid = ParsePid(argv[i]);
            if (pid <= 0 || Process.Kill((uint)pid) != 0)
            {
                Console.Write("kill: no process ");
                Console.Write(argv[i]);
                Console.WriteLine();
                failed++;
            }
        }
        return failed == 0 ? 0 : 1;
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
