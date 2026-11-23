// sh — a shell in ring 3. Runs programs from /bin (NAME ARGS…) or by path (./prog, /tmp/prog);
// `&` at the end runs in the background; `> FILE`, `>> FILE` and `< FILE` redirect stdout / stdin;
// `a | b | c` connects them with pipes.
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
            if (HasPipe(name))
            {
                RunPipeline(name);
                continue;
            }
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
        Console.WriteLine("           A | B | C: pipes, e.g.  ls /bin | grep s | wc   ·   cat /etc/motd | head -n 2");
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

    static bool HasPipe(byte* s)
    {
        for (; *s != 0; s++)
        {
            if (*s == '|')
                return true;
        }
        return false;
    }

    const int MaxStages = 8;

    /// A | B | C: every stage starts with stdin from the previous pipe and stdout into the next one
    /// (the shell's own 0/1 are switched around each spawn and put back), then the shell waits for all.
    static void RunPipeline(byte* line)
    {
        TrimEnd(line);
        int n = (int)Str.Length(line);
        bool background = n > 0 && line[n - 1] == '&';
        if (background)
        {
            line[n - 1] = 0;
            TrimEnd(line);
        }

        byte** stages = stackalloc byte*[MaxStages];
        int count = 0;
        stages[count++] = line;
        for (byte* p = line; *p != 0; p++)
        {
            if (*p != '|')
                continue;
            if (count == MaxStages)
            {
                Console.WriteLine(Ansi.Red, "sh: at most 8 commands in a pipeline");
                return;
            }
            *p = 0;
            stages[count++] = p + 1;
        }

        long* pids = stackalloc long[MaxStages];
        byte** names = stackalloc byte*[MaxStages];
        long savedIn = File.Duplicate(0);
        long savedOut = File.Duplicate(1);
        int previousRead = -1;
        bool ok = true;
        for (int i = 0; i < count; i++)
        {
            pids[i] = -1;
            byte* name = stages[i];
            while (*name == ' ')
                name++;
            byte* args = name;
            while (*args != 0 && *args != ' ')
                args++;
            if (*args != 0)
            {
                *args = 0;
                args++;
            }
            names[i] = name;
            TrimEnd(args);
            if (*name == 0 || !ParseRedirections(args))
            {
                Console.WriteLine(Ansi.Red, "sh: empty command or a redirection without a file in the pipeline");
                ok = false;
            }
            TrimEnd(args);

            int readEnd = -1, writeEnd = -1;
            if (ok && i < count - 1 && File.Pipe(out readEnd, out writeEnd) < 0)
            {
                Console.WriteLine(Ansi.Red, "sh: cannot create a pipe");
                ok = false;
            }
            if (ok)
            {
                // stdin: the previous stage, or `< FILE`; stdout: the next stage, or `> FILE`.
                long input = previousRead >= 0 ? previousRead : -1;
                if (_stdinPath != null)
                    input = File.Open(_stdinPath, File.Read);
                long output = writeEnd >= 0 ? writeEnd : -1;
                if (_stdoutPath != null)
                    output = File.Open(_stdoutPath, File.Write | File.Create | (_append ? File.Append : File.Truncate));
                if ((_stdinPath != null && input < 0) || (_stdoutPath != null && output < 0))
                {
                    Tool.Fail("sh", _stdinPath != null && input < 0 ? _stdinPath : _stdoutPath, input < 0 ? input : output);
                    ok = false;
                }
                else
                {
                    if (input >= 0)
                        File.Redirect((int)input, 0);
                    if (output >= 0)
                        File.Redirect((int)output, 1);
                    pids[i] = Process.Start(name, args);
                    File.Redirect((int)savedIn, 0);
                    File.Redirect((int)savedOut, 1);
                    if (pids[i] < 0)
                    {
                        Console.Write(Ansi.Red);
                        Console.Write("sh: ");
                        Console.Write(name);
                        Console.Write(": ");
                        Console.Write(pids[i] == Process.ErrNotFound ? "not found" : Process.ErrorText(pids[i]));
                        Console.WriteLine(Ansi.Reset);
                    }
                }
                if (_stdinPath != null && input >= 0)
                    File.Close((int)input);
                if (_stdoutPath != null && output >= 0)
                    File.Close((int)output);
            }
            // The children hold their own ends now: ours must go, or readers would never see the end.
            if (previousRead >= 0)
                File.Close(previousRead);
            if (writeEnd >= 0)
                File.Close(writeEnd);
            previousRead = readEnd;
        }
        if (previousRead >= 0)
            File.Close(previousRead);
        File.Close((int)savedIn);
        File.Close((int)savedOut);

        for (int i = 0; i < count; i++)
        {
            if (pids[i] < 0)
                continue;
            if (background)
            {
                Console.WriteLine($"[{pids[i]}]");
                continue;
            }
            long code = Process.Wait((uint)pids[i]);
            // A writer that outlived its reader ends with 141 (broken pipe): normal for `… | head`.
            if (code != Process.ExitBrokenPipe || i == count - 1)
                Report(names[i], code);
        }
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
