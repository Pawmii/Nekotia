// sh — a shell in ring 3. Runs programs from /bin (NAME ARGS…) or by path (./prog, /tmp/prog);
// `&` at the end runs in the background; `> FILE`, `>> FILE` and `< FILE` redirect stdout / stdin.
// Built-ins: help, cd [DIR], pwd, exit [CODE], wait PID, pid. Ctrl+C ends the program in the
// foreground, Ctrl+D ends sh.
using Nekotia;
using Nekotia.User;

namespace ShProgram;

public static unsafe class Program
{
    const int LineSize = 256;

    public static int Main()
    {
        byte* line = stackalloc byte[LineSize];
        _outBuffer = (byte*)Memory.Alloc(LineSize);
        _inBuffer = (byte*)Memory.Alloc(LineSize);
        Console.Write(Ansi.Cyan);
        Console.Write($"sh — user-mode shell, pid {Process.Id()}. `help` lists the built-ins, Ctrl+D leaves.");
        Console.WriteLine(Ansi.Reset);
        while (true) {
            Prompt();
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
            else if (Str.Equals(name, "cd"))
                ChangeDirectory(args);
            else if (Str.Equals(name, "pwd"))
                PrintDirectory();
            else
                Run(name, args);
        }
    }

    static void Prompt()
    {
        byte* cwd = stackalloc byte[File.MaxPath];
        if (File.CurrentDirectory(cwd, File.MaxPath) < 0)
            cwd[0] = 0;
        Console.Write(Ansi.Green, "sh ");
        Console.Write(Ansi.Blue);
        Console.Write(cwd);
        Console.Write(Ansi.Green, "$ ");
    }

    static void Help()
    {
        Console.WriteLine("built-ins: help, cd [DIR], pwd, exit [CODE], wait PID, pid");
        Console.WriteLine("programs:  NAME ARGS…   (/bin/NAME, or a path like ./prog; `ls /bin` lists them)");
        Console.WriteLine("           `&` at the end: background;  > FILE, >> FILE: stdout to FILE;  < FILE: stdin from FILE");
        Console.WriteLine("Ctrl+C ends the program in the foreground, Ctrl+D (on an empty line) leaves sh.");
    }

    static void ChangeDirectory(byte* args)
    {
        byte* path = *args != 0 ? args : Str.Bytes("/");
        TrimEnd(path);
        long e = File.ChangeDirectory(path);
        if (e < 0)
            Tool.Fail("cd", path, e);
    }

    static void PrintDirectory()
    {
        byte* cwd = stackalloc byte[File.MaxPath];
        long e = File.CurrentDirectory(cwd, File.MaxPath);
        if (e < 0)
        {
            Tool.Fail("pwd", null, e);
            return;
        }
        Console.Write(cwd);
        Console.WriteLine();
    }

    static void TrimEnd(byte* s)
    {
        int n = (int)Str.Length(s);
        while (n > 0 && s[n - 1] == ' ')
            s[--n] = 0;
    }

    // Redirections found in the arguments (copied out of them).
    static byte* _outBuffer;
    static byte* _inBuffer;
    static byte* _stdoutPath;
    static bool _append;
    static byte* _stdinPath;

    /// Takes `> FILE`, `>> FILE` and `< FILE` out of `args`; false on a missing file name.
    static bool ParseRedirections(byte* args)
    {
        _stdoutPath = null;
        _stdinPath = null;
        _append = false;
        byte* read = args;
        byte* write = args;
        while (*read != 0)
        {
            if (*read == '>' || *read == '<')
            {
                bool output = *read == '>';
                read++;
                bool append = output && *read == '>';
                if (append)
                    read++;
                while (*read == ' ')
                    read++;
                if (*read == 0 || *read == '>' || *read == '<')
                    return false;
                byte* path = output ? _outBuffer : _inBuffer;
                int length = 0;
                while (*read != 0 && *read != ' ')
                {
                    if (length < LineSize - 1)
                        path[length++] = *read;
                    read++;
                }
                path[length] = 0;
                if (output)
                {
                    _stdoutPath = path;
                    _append = append;
                }
                else
                {
                    _stdinPath = path;
                }
                continue;
            }
            *write++ = *read++;
        }
        *write = 0;
        return true;
    }

    /// Opens `path` and puts it on `target` (0 or 1); returns the saved original fd, or -1 after an error.
    static long Redirect(byte* path, int target, uint flags)
    {
        long fd = File.Open(path, flags);
        if (fd < 0)
        {
            Tool.Fail("sh", path, fd);
            return -1;
        }
        long saved = File.Duplicate(target);
        File.Redirect((int)fd, target);
        File.Close((int)fd);
        return saved;
    }

    static void Restore(long saved, int target)
    {
        if (saved < 0)
            return;
        File.Redirect((int)saved, target);
        File.Close((int)saved);
    }

    static void Run(byte* name, byte* args)
    {
        bool background = false;
        TrimEnd(args);
        int n = (int)Str.Length(args);
        if (n > 0 && args[n - 1] == '&')
        {
            background = true;
            args[n - 1] = 0;
            TrimEnd(args);
        }
        if (!ParseRedirections(args))
        {
            Console.WriteLine(Ansi.Red, "sh: a redirection needs a file name");
            return;
        }
        TrimEnd(args);

        long savedOut = -1;
        long savedIn = -1;
        if (_stdoutPath != null)
        {
            savedOut = Redirect(_stdoutPath, 1, File.Write | File.Create | (_append ? File.Append : File.Truncate));
            if (savedOut < 0)
                return;
        }
        if (_stdinPath != null)
        {
            savedIn = Redirect(_stdinPath, 0, File.Read);
            if (savedIn < 0)
            {
                Restore(savedOut, 1);
                return;
            }
        }
        long pid = Process.Start(name, args);
        // The child has its own references to the redirected files: ours go back to the console.
        Restore(savedOut, 1);
        Restore(savedIn, 0);
        if (pid < 0)
        {
            Console.Write(Ansi.Red);
            Console.Write("sh: ");
            Console.Write(name);
            Console.Write(": ");
            Console.Write(pid == Process.ErrNotFound ? "not found" : Process.ErrorText(pid));
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
