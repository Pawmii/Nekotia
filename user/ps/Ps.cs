// ps — the kernel's process table, seen from ring 3 (process_info system call).
using Nekotia;
using Nekotia.User;

namespace PsProgram;

public static unsafe class Program
{
    public static int Main()
    {
        Console.WriteLine(Ansi.Cyan, "  PID  PARENT  NAME                 THREADS  MEMORY     RUNNING FOR");
        ProcessInfo p;
        for (uint i = 0; Process.GetInfo(i, out p); i++)
        {
            Console.Write($"  {p.Pid}");
            Pad(Digits(p.Pid), 5);
            if (p.Parent == 0)
                Console.Write("kernel");
            else
                Console.Write($"{p.Parent}");
            Pad(p.Parent == 0 ? 6 : Digits(p.Parent), 8);
            Console.Write(p.Name);
            Pad((int)Str.Length(p.Name), 21);
            Console.Write($"{p.Threads}");
            Pad(Digits(p.Threads), 9);
            Console.Write($"{p.MemoryKib} KiB");
            Pad(Digits(p.MemoryKib) + 4, 11);
            Console.WriteLine($"{p.UptimeMs} ms{(p.Pid == Process.Id() ? "  ← me" : "")}");
        }
        return 0;
    }

    static void Pad(int used, int width)
    {
        for (int i = used; i < width; i++)
            Console.WriteChar((byte)' ');
    }

    static int Digits(ulong v)
    {
        int n = 1;
        while (v >= 10)
        {
            v /= 10;
            n++;
        }
        return n;
    }
}
