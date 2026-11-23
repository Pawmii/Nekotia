// Checks of the user-mode ABI, run by the kernel self test (kernel/SelfTest.neko):
//   usertest [-v] ARGS…   system calls, arguments, heap, FPU, child processes → exit code 42 when all pass
//   usertest child        exits with 7 (for spawn/wait)
//   usertest pf | gp | ud | kernel   faults on purpose; the kernel must kill only this process
//   usertest spin | sleep | read     never finishes on its own (for kill and Ctrl+C tests)
//   usertest parent                  waits for a `usertest sleep` child forever
//   usertest texit | tfault          a second thread calls exit(9) / dereferences null
//   usertest cwd                     prints its working directory (for the inheritance check)
//   usertest flood                   writes to stdout forever (broken-pipe check)
//   usertest churn                   maps fresh memory in a loop, for many copies at once (allocator stress)
//   usertest piperead                waits on a pipe nobody writes to (kill check)
using Nekotia;
using Nekotia.User;

namespace UserTest;

public static unsafe class Program
{
    const ulong KernelAddress = 0xffffffff80000000UL;

    static int _passed;
    static int _failed;
    static bool _verbose;

    public static int Main(int argc, byte** argv)
    {
        byte* mode = argc > 1 ? argv[1] : null;
        if (mode != null)
        {
            if (Str.Equals(mode, "child"))
                return 7;
            if (Str.Equals(mode, "pf"))
                return *(int*)0;                       // NullReferenceException → #PF
            if (Str.Equals(mode, "kernel"))
                return *(int*)KernelAddress;           // kernel memory from ring 3 → #PF
            if (Str.Equals(mode, "gp"))
            {
                Asm.Emit("cli");                       // privileged instruction → #GP
                return 1;
            }
            if (Str.Equals(mode, "ud"))
            {
                Asm.Emit("ud2");                       // invalid opcode → #UD
                return 1;
            }
            if (Str.Equals(mode, "spin"))
                while (true) { }
            if (Str.Equals(mode, "sleep"))
                while (true) { Process.Sleep(100000); }
            if (Str.Equals(mode, "read"))
            {
                byte* line = stackalloc byte[16];
                return (int)Console.ReadLine(line, 16);
            }
            if (Str.Equals(mode, "parent"))
            {
                long child = Process.Start("usertest", "sleep");
                return (int)Process.Wait((uint)child);
            }
            if (Str.Equals(mode, "texit") || Str.Equals(mode, "tfault"))
            {
                Thread.Start(Str.Equals(mode, "texit") ? &ExitFromThread : &FaultInThread);
                while (true) { Process.Sleep(100000); }
            }
            if (Str.Equals(mode, "churn"))
            {
                for (int round = 0; round < 40; round++)
                {
                    byte* p = (byte*)Memory.Alloc(64 * 1024 + (ulong)round * 4096);   // grows: new pages from the kernel
                    if (p == null)
                        return 1;
                    for (int i = 0; i < 64 * 1024; i += 4096)
                        p[i] = (byte)i;
                    Memory.Free(p);
                }
                return 0;
            }
            if (Str.Equals(mode, "flood"))
            {
                byte* chunk = stackalloc byte[512];
                while (true)
                    File.WriteBytes(1, chunk, 512);
            }
            if (Str.Equals(mode, "piperead"))
            {
                int r = 0, w = 0;
                File.Pipe(out r, out w);
                byte* b = stackalloc byte[4];
                return (int)File.ReadBytes(r, b, 4);
            }
            if (Str.Equals(mode, "cwd"))
            {
                byte* cwd = stackalloc byte[File.MaxPath];
                File.CurrentDirectory(cwd, File.MaxPath);
                Console.Write(cwd);
                return 0;
            }
            _verbose = Str.Equals(mode, "-v");
        }

        Arguments(argc, argv);
        SystemCalls();
        BadPointers();
        HeapAndFpu();
        Children();
        Threads();
        Files();
        Pipes();

        if (_verbose || _failed > 0)
            Console.WriteLine($"usertest: {_passed} passed, {_failed} failed");
        return _failed == 0 ? 42 : 1;
    }

    static void Check(bool ok, string name)
    {
        if (ok)
            _passed++;
        else
            _failed++;
        if (ok && !_verbose)
            return;
        Console.Write(ok ? Ansi.Green : Ansi.Red, ok ? "  ok    " : "  FAIL  ");
        Console.WriteLine(name);
    }

    static void Arguments(int argc, byte** argv)
    {
        int first = _verbose ? 2 : 1;
        Check(Str.Equals(argv[0], "usertest"), "argv[0] is the program name");
        Check(argc == first + 2 && Str.Equals(argv[first], "alpha") && Str.Equals(argv[first + 1], "beta") && argv[argc] == null,
            "arguments arrive split, NULL-terminated");
        Check(Str.Equals(Process.Arg(first), "alpha") && Process.ArgCount() == argc, "Process.Arg mirrors argv");
    }

    static void SystemCalls()
    {
        Check(Process.Id() > 0, "getpid");
        ulong t0 = Time.UptimeMs();
        Process.Sleep(10);
        ulong slept = Time.UptimeMs() - t0;
        Check(slept >= 10 && slept < 100, "sleep(10) takes ~10 ms");
        Process.Yield();
        Check(Process.Syscall(50) == Process.ErrNoSyscall, "unknown system call → ErrNoSyscall");

        bool found = false;
        ProcessInfo info;
        for (uint i = 0; Process.GetInfo(i, out info); i++)
            if (info.Pid == Process.Id() && Str.Equals(info.Name, "usertest") && info.MemoryKib > 0)
                found = true;
        Check(found, "process_info lists this process");
    }

    static void BadPointers()
    {
        Check(Process.Syscall(1, 1, KernelAddress, 16) == Process.ErrBadAddress, "write() refuses a kernel pointer");
        Check(Process.Syscall(1, 1, 0x1000, 16) == Process.ErrBadAddress, "write() refuses an unmapped pointer");
        Check(Process.Syscall(2, 0, KernelAddress, 16) == Process.ErrBadAddress, "read() refuses a kernel buffer");
        Check(Process.Syscall(2, 0, 0x400000, 16) == Process.ErrBadAddress, "read() refuses read-only memory (code)");
        Check(Process.Syscall(7, KernelAddress, 0) == Process.ErrBadAddress, "spawn() refuses a kernel string");
        Check(Process.Start("no-such-program") == Process.ErrNotFound, "spawn() of a missing program → ErrNotFound");
        Check(Process.Syscall(10, 0, 0x1000) == Process.ErrBadAddress, "a page fault while the kernel copies out fails the call, not the kernel");
        Check(Process.Syscall(10, 0, 0x400000) == Process.ErrBadAddress, "the kernel cannot write into read-only program memory");
    }

    static void HeapAndFpu()
    {
        byte* big = (byte*)Memory.Alloc(1 << 20);
        bool ok = big != null;
        if (ok)
        {
            for (int i = 0; i < 1 << 20; i += 4096)
                big[i] = (byte)(i >> 12);
            for (int i = 0; i < 1 << 20; i += 4096)
                ok &= big[i] == (byte)(i >> 12);
        }
        Check(ok, "1 MiB from the heap is usable");
        ulong used = Memory.UsedBytes();
        Memory.Free(big);
        void* again = Memory.Alloc(1 << 20);
        Check(again == big && Memory.UsedBytes() == used, "freed memory is reused");
        Memory.Free(again);

        double x = Maths.Sqrt(2.0);
        double y = x * 3.0;
        Process.Yield();          // another thread runs; SSE registers must survive
        Process.Sleep(1);
        Check(Maths.Abs(x * x - 2.0) < 1e-12 && Maths.Abs(y / x - 3.0) < 1e-12, "floating point survives system calls and switches");
    }

    static void Children()
    {
        long child = Process.Start("usertest", "child");
        Check(child > 0 && Process.Wait((uint)child) == 7, "spawn + wait: the child's exit code");
        Check(Process.Wait((uint)child) == Process.ErrNotFound, "a child can be waited for only once");
        long other = Process.Wait(Process.Id());
        Check(other == Process.ErrNotChild, "wait() for a process that is not a child fails");
    }

    // ---------------------------------------------------------------- files

    static long ReadAll(byte* path, byte* buffer, ulong capacity)
    {
        long fd = File.Open(path, File.Read);
        if (fd < 0)
            return fd;
        long n = File.ReadBytes((int)fd, buffer, capacity - 1);
        File.Close((int)fd);
        if (n >= 0)
            buffer[n] = 0;
        return n;
    }

    static void Files()
    {
        byte* buf = stackalloc byte[256];
        FileStat st = default;
        Check(File.CreateDirectory("/tmp/usertest") == 0 || File.IsDirectory(Str.Bytes("/tmp/usertest")), "mkdir /tmp/usertest");
        long fd = File.Open("/tmp/usertest/f", File.Read | File.Write | File.Create | File.Truncate);
        Check(fd >= 3, "open: a new file gets the lowest free fd above 0–2");
        Check(File.WriteBytes((int)fd, Str.Bytes("nekotia"), 7) == 7 && File.Seek((int)fd, 0, File.SeekSet) == 0
              && File.ReadBytes((int)fd, buf, 16) == 7 && buf[0] == 'n' && buf[6] == 'a', "write, seek, read back");
        Check(File.Stat((int)fd, out st) == 0 && st.Size == 7 && st.Type == (byte)NodeType.File, "fstat");
        long copy = File.Duplicate((int)fd);
        Check(copy > fd && File.Seek((int)copy, 2, File.SeekSet) == 2 && File.Seek((int)fd, 0, File.SeekCurrent) == 2,
              "dup shares the file offset");
        Check(File.Close((int)copy) == 0 && File.Close((int)copy) == Process.ErrBadFd, "close; closing twice fails");
        File.Close((int)fd);
        Check(File.ReadBytes(17, buf, 4) == Process.ErrBadFd && File.ReadBytes(999, buf, 4) == Process.ErrBadFd, "bad fds are refused");
        Check(Process.Syscall(16, KernelAddress, File.Read) == Process.ErrBadAddress, "open() refuses a kernel string");
        Check(Process.Syscall(19, (ulong)Str.Bytes("/tmp"), KernelAddress) == Process.ErrBadAddress, "stat() refuses a kernel buffer");

        // A child inherits stdout: redirect it into a file around spawn.
        long outFd = File.Open("/tmp/usertest/out", File.Write | File.Create | File.Truncate);
        long saved = File.Duplicate(1);
        File.Redirect((int)outFd, 1);
        File.Close((int)outFd);
        long child = Process.Start("echo", "inherited stdout");
        File.Redirect((int)saved, 1);
        File.Close((int)saved);
        Check(child > 0 && Process.Wait((uint)child) == 0 && ReadAll(Str.Bytes("/tmp/usertest/out"), buf, 256) == 17
              && Str.StartsWith(buf, "inherited stdout"), "a child writes to the stdout it inherited (redirection)");

        Check(File.ChangeDirectory("/tmp/usertest") == 0 && File.Exists(Str.Bytes("f")), "chdir + relative path");
        outFd = File.Open("cwd.txt", File.Write | File.Create | File.Truncate);
        saved = File.Duplicate(1);
        File.Redirect((int)outFd, 1);
        File.Close((int)outFd);
        child = Process.Start("usertest", "cwd");
        File.Redirect((int)saved, 1);
        File.Close((int)saved);
        Check(child > 0 && Process.Wait((uint)child) == 0 && ReadAll(Str.Bytes("cwd.txt"), buf, 256) > 0
              && Str.Equals(buf, "/tmp/usertest"), "a child starts in its parent's working directory");

        int opened = 0;
        long last = 0;
        long* fds = stackalloc long[40];
        for (int i = 0; i < 40; i++)
        {
            fds[i] = File.Open("f", File.Read);
            if (fds[i] >= 0)
                opened++;
            else
                last = fds[i];
        }
        Check(opened == 29 && last == Process.ErrTooManyFiles, "32 file descriptors per process, then ErrTooManyFiles");
        for (int i = 0; i < 40; i++)
        {
            if (fds[i] >= 0)
                File.Close((int)fds[i]);
        }

        long dir = File.Open(".", File.Read);
        DirEntry e = default;
        uint entries = 0;
        for (uint i = 0; File.ReadDirectory((int)dir, i, out e) == 1; i++)
            entries++;
        File.Close((int)dir);
        Check(entries == 3, "readdir lists f, out, cwd.txt");
        Check(File.Delete("f") == 0 && File.Delete("out") == 0 && File.Delete("cwd.txt") == 0 && File.ChangeDirectory("/") == 0
              && File.Delete("/tmp/usertest") == 0, "remove files and the directory");
        Check(ReadAll(Str.Bytes("/etc/motd"), buf, 8) == 7 && Str.StartsWith(buf, "Things "), "read a file from the initrd");
    }

    // ---------------------------------------------------------------- pipes

    const int PipeBytes = 200000;   // more than the 64 KiB buffer: the writer has to wait (whole 1000-byte chunks)
    static int _pipeWriter;

    static void PipeWriter(ulong unused)
    {
        byte* chunk = stackalloc byte[1000];
        for (int sent = 0; sent < PipeBytes; sent += 1000)
        {
            for (int i = 0; i < 1000; i++)
                chunk[i] = (byte)((sent + i) * 7);
            File.WriteBytes(_pipeWriter, chunk, 1000);
        }
        File.Close(_pipeWriter);
    }

    static void Pipes()
    {
        byte* buf = stackalloc byte[4096];
        int r = 0, w = 0;
        Check(File.Pipe(out r, out w) == 0 && r >= 3 && w > r, "pipe() gives two new fds");
        Check(File.WriteBytes(w, Str.Bytes("hey"), 3) == 3 && File.ReadBytes(r, buf, 16) == 3 && buf[2] == 'y', "write → read");

        // A child's stdout into the pipe.
        long saved = File.Duplicate(1);
        File.Redirect(w, 1);
        long child = Process.Start("echo", "through a pipe");
        File.Redirect((int)saved, 1);
        File.Close((int)saved);
        File.Close(w);   // the child holds the only write end now
        long got = 0;
        while (true)
        {
            long n = File.ReadBytes(r, buf + got, 64);
            if (n <= 0)
                break;
            got += n;
        }
        buf[got] = 0;
        Check(child > 0 && Process.Wait((uint)child) == 0 && got == 15 && Str.StartsWith(buf, "through a pipe"),
              "a child's output arrives through a pipe, then end of input");
        File.Close(r);

        // 200 KiB from a second thread: the writer blocks on the full buffer, the reader drains it.
        File.Pipe(out r, out _pipeWriter);
        ulong t = Thread.Start(&PipeWriter);
        long total = 0;
        bool same = true;
        while (true)
        {
            long n = File.ReadBytes(r, buf, 4096);
            if (n <= 0)
                break;
            for (long i = 0; i < n; i++)
                same &= buf[i] == (byte)((total + i) * 7);
            total += n;
        }
        Thread.Join(t);
        File.Close(r);
        if (total != PipeBytes || !same)
            Console.WriteLine($"  pipe: got {total} bytes, in order: {(same ? 1 : 0)}");
        Check(total == PipeBytes && same, "200 KB through a 64 KiB pipe, in order");

        // Nobody reads: the writer is ended with 141 (like SIGPIPE).
        File.Pipe(out r, out w);
        File.Close(r);
        saved = File.Duplicate(1);
        File.Redirect(w, 1);
        child = Process.Start("usertest", "flood");
        File.Redirect((int)saved, 1);
        File.Close((int)saved);
        File.Close(w);
        Check(child > 0 && Process.Wait((uint)child) == Process.ExitBrokenPipe, "writing to a pipe without readers ends the writer (141)");
    }

    static void ExitFromThread(ulong arg)
    {
        Process.Sleep(5);
        Process.Exit(9);
    }

    static void FaultInThread(ulong arg)
    {
        Process.Sleep(5);
        *(int*)0 = 1;
    }

    // ---------------------------------------------------------------- threads

    const int Workers = 4;
    const int Rounds = 20000;

    static long* _atomicCounter;
    static long* _lockedCounter;
    static SpinLock _lock;

    static void Worker(ulong id)
    {
        for (int i = 0; i < Rounds; i++)
        {
            Atomic.Add(_atomicCounter, 1);
            _lock.Lock();
            *_lockedCounter = *_lockedCounter + 1;   // a plain read-modify-write, safe only under the lock
            _lock.Unlock();
        }
        void* p = Memory.Alloc(100 + id);           // the heap is shared and locked
        Memory.Free(p);
    }

    static void Threads()
    {
        _atomicCounter = Memory.New<long>();
        _lockedCounter = Memory.New<long>();
        ulong* handles = stackalloc ulong[Workers];
        for (int i = 0; i < Workers; i++)
            handles[i] = Thread.Start(&Worker, (ulong)i);
        bool started = true;
        for (int i = 0; i < Workers; i++)
            started &= handles[i] != 0;
        for (int i = 0; i < Workers; i++)
            Thread.Join(handles[i]);
        Check(started, "4 threads start");
        Check(*_atomicCounter == Workers * Rounds && *_lockedCounter == Workers * Rounds,
            "threads share memory: atomic and locked counters are exact");
        Check(Thread.CurrentId() != 0, "thread_id");
    }
}
