// sh — a shell in ring 3. Runs programs from /boot/bin (NAME ARGS…, `&` at the end: in the background),
// built-ins: help, exit [CODE], wait PID, pid. Ctrl+C ends the program in the foreground, Ctrl+D ends sh.
using Nekotia;
using Nekotia.User;

namespace ShProgram;

public static unsafe class Program
{
    const int LineSize = 256;

    public static int Main()
    {
        byte* line = stackalloc byte[LineSize];
        Console.Write(Ansi.Cyan);
        Console.Write($"sh — user-mode shell, pid {Process.Id()}. `help` lists the built-ins, Ctrl+D leaves.");
        Console.WriteLine(Ansi.Reset);
        while (true) {
            Console.Write(Ansi.Green, "sh$ ");
            if (Console.ReadLine(line, LineSize) < 0)
            {
                Console.WriteLine();
                return 0;
            }

            byte* name = line;
            while (*name == ' ')
                name++;
            if (*name == 0)
                continue;
            byte* args = name;
            while (*args != 0 && *args != ' ')
                args++;
            if (*args != 0)
            {
                *args = 0;
                args++;
                while (*args == ' ')
                    args++;
            }

            if (Str.Equals(name, "exit"))
                return (int)Number(args, 0);
            if (Str.Equals(name, "help"))
                Help();
            else if (Str.Equals(name, "pid"))
                Console.WriteLine($"{Process.Id()}");
            else if (Str.Equals(name, "wait"))
                Report(name, Process.Wait((uint)Number(args, 0)));
            else
                Run(name, args);
        }
    }

    static void Help()
    {
        Console.WriteLine("built-ins: help, exit [CODE], wait PID, pid");
        Console.WriteLine("programs:  NAME ARGS…   (`&` at the end runs it in the background; `ls` lists them)");
        Console.WriteLine("Ctrl+C ends the program in the foreground, Ctrl+D (on an empty line) leaves sh.");
    }

    static void Run(byte* name, byte* args)
    {
        bool background = false;
        int n = (int)Str.Length(args);
        while (n > 0 && args[n - 1] == ' ')
            n--;
        if (n > 0 && args[n - 1] == '&')
        {
            background = true;
            args[n - 1] = 0;
        }
        long pid = Process.Start(name, args);
        if (pid < 0)
        {
            Console.Write(Ansi.Red);
            Console.Write("sh: ");
            Console.Write(name);
            Console.Write(pid == Process.ErrNotFound ? ": not found" : ": cannot start");
            Console.WriteLine(Ansi.Reset);
            return;
        }
        if (background)
            Console.WriteLine($"[{pid}]");
        else
            Report(name, Process.Wait((uint)pid));
    }

    static void Report(byte* name, long code)
    {
        if (code == 0)
            return;
        Console.Write(Ansi.Gray);
        Console.Write("[");
        Console.Write(name);
        Console.Write($": {code}]");
        Console.WriteLine(Ansi.Reset);
    }

    static long Number(byte* s, long fallback)
    {
        long v = 0;
        if (*s == 0) return fallback;
        while (*s >= '0' && *s <= '9')
        {
            v = v * 10 + (*s - '0');
            s++;
        }
        return v;
    }
}
