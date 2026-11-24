// Checks of the user-mode ABI, run by the kernel self test (kernel/SelfTest.neko):
//   usertest [-v] ARGS…   system calls, arguments, heap, FPU, child processes → exit code 42 when all pass
//   usertest child        exits with 7 (for spawn/wait)
//   usertest pf | gp | ud | kernel   faults on purpose; the kernel must kill only this process
//   usertest spin | sleep | read     never finishes on its own (for kill and Ctrl+C tests)
//   usertest parent                  waits for a `usertest sleep` child forever
//   usertest texit | tfault          a second thread calls exit(9) / dereferences null
//   usertest cwd                     prints its working directory (for the inheritance check)
//   usertest flood                   writes to stdout forever (broken-pipe check)
//   usertest unmapped | readonly     touches unmapped / read-only pages (exit code 142)
//   usertest uncaught                throws without a try (exit code 134)
//   usertest churn                   maps fresh memory in a loop, for many copies at once (allocator stress)
//   usertest piperead                waits on a pipe nobody writes to (kill check)
//   usertest ipcchild                the other side of the IPC check: connects, writes into shared memory it is sent
//   usertest sigterm                 waits for SIGTERM with a handler, then exits 5
//   usertest sigspin                 computes for a while (integers and doubles) while signals rain on it; 0 = intact
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
            if (Str.Equals(mode, "unmapped"))
            {
                byte* page = (byte*)Memory.MapPages(4096);
                page[0] = 1;
                Memory.UnmapPages(page, 4096);
                return page[0];   // #PF → 142
            }
            if (Str.Equals(mode, "readonly"))
            {
                byte* page = (byte*)Memory.MapPages(4096);
                page[0] = 1;
                Memory.ProtectPages(page, 4096, Memory.ReadOnly);
                byte v = page[0];   // still readable
                page[0] = (byte)(v + 1);   // #PF → 142
                return 0;
            }
            if (Str.Equals(mode, "uncaught"))
                throw new System.InvalidOperationException("nobody catches this");
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
            if (Str.Equals(mode, "ipcchild"))
                return IpcChild();
            if (Str.Equals(mode, "sigterm"))
            {
                Signal.Handle(Signal.Term, &OnTerm);
                while (_signals == 0)
                    Process.Sleep(1000);
                return 5;
            }
            if (Str.Equals(mode, "sigspin"))
                return SigSpin();
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
        Sockets();
        Exceptions();
        FutexAndPages();
        DemandPaging();
        Signals();
        Ipc();
        Devices();

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

    // ---------------------------------------------------------------- shared memory, channels, poll

    static void Ipc()
    {
        byte* buf = stackalloc byte[64];
        // shared memory as a file and as memory
        long shm = SharedMemory.Create(5000);
        FileStat st = default;
        Check(shm >= 3 && File.Stat((int)shm, out st) == 0 && st.Size == 8192, "shm_create: a zeroed object of whole pages");
        File.WriteBytes((int)shm, Str.Bytes("neko"), 4);
        long at = Device.Map((int)shm, 8192, Device.MapWrite);
        byte* m = (byte*)at;
        Check(at > 0 && m[0] == 'n' && m[3] == 'o' && m[4] == 0 && m[8191] == 0, "mmap of shared memory sees what write() put there");
        m[4100] = 42;
        File.Seek((int)shm, 4100, File.SeekSet);
        Check(File.ReadBytes((int)shm, buf, 1) == 1 && buf[0] == 42, "…and read() sees what the mapping wrote");
        Check(Memory.ProtectPages(m, 4096, Memory.ReadOnly) < 0, "shared pages keep the rights they were mapped with (protect refused)");
        Check(Device.Map(0, 4096, 0) < 0, "mmap of something that is not mappable fails");

        // channels: a child gets the shared memory through a message and writes into it
        long listener = Channel.Listen("usertest-ipc");
        Check(listener >= 0 && Channel.Listen("usertest-ipc") == Process.ErrExists, "ipc_listen: a name has one owner");
        Check(Channel.Connect("usertest-nobody") == Process.ErrNotFound, "ipc_connect to a name nobody listens on fails");
        PollFd p = default;
        p.Fd = (int)listener;
        p.Events = Poll.In;
        Check(Poll.Wait(&p, 1, 0) == 0 && p.Ready == 0, "poll: a listener without connections is not ready");
        long child = Process.Start("usertest", "ipcchild");
        p.Ready = 0;
        Check(Poll.Wait(&p, 1, 3000) == 1 && (p.Ready & Poll.In) != 0, "poll: wakes when a connection arrives");
        long conn = Channel.Accept((int)listener);
        Check(conn >= 0, "ipc_accept gives the server end");
        Check(Channel.Send((int)conn, Str.Bytes("map this"), 8, (int)shm) == 8, "ipc_send with an fd attached");
        p.Fd = (int)conn;
        p.Ready = 0;
        int fdOut = -1;
        long n = Poll.Wait(&p, 1, 3000) == 1 ? Channel.Receive((int)conn, buf, 64, &fdOut) : -1;
        Check(n == 4 && buf[0] == 'd' && fdOut == -1, "the reply arrives as one message, without a file");
        Check(m[0] == 'p' && m[1] == 'o' && m[2] == 'n' && m[3] == 'g', "the child wrote into the same pages (shared across address spaces)");
        Check(child > 0 && Process.Wait((uint)child) == 0, "the child exits cleanly");
        p.Ready = 0;
        Check(Poll.Wait(&p, 1, 1000) == 1 && (p.Ready & Poll.Hup) != 0 && Channel.Receive((int)conn, buf, 64, &fdOut) == 0,
              "after the peer exits: poll says hang-up, receive says end");
        Check(Channel.Send((int)conn, buf, 1, -1) == Process.ErrBrokenPipe, "sending to a gone peer fails (without killing the sender)");
        Check(Device.Unmap(m) == 0 && Device.Unmap(m) < 0, "munmap removes the mapping once");
        File.Close((int)conn);
        File.Close((int)listener);
        File.Close((int)shm);
        Check(Channel.Connect("usertest-ipc") == Process.ErrNotFound, "closing the listener frees the name");
    }

    // ---------------------------------------------------------------- signals

    static volatile int _signals;

    static void OnSignal(int sig) => _signals += sig;

    static void OnTerm(int sig) => _signals = 1;

    static void SendLater(ulong pid)
    {
        Process.Sleep(50);
        Signal.Send((uint)pid, Signal.Usr2);
    }

    static void Signals()
    {
        _signals = 0;
        Check(Signal.Handle(Signal.Usr1, &OnSignal) == 0 && Signal.Send(Process.Id(), Signal.Usr1) == 0 && _signals == Signal.Usr1,
              "signals: a handler runs (on our way back from the kernel)");
        Signal.Handle(Signal.Usr2, &OnSignal);
        ulong t = Thread.Start(&SendLater, Process.Id());
        ulong start = Time.UptimeMs();
        Process.Sleep(5000);
        ulong slept = Time.UptimeMs() - start;
        Thread.Join(t);
        Check(slept < 2000 && _signals == Signal.Usr1 + Signal.Usr2, "signals: a signal cuts a sleep short");
        Signal.Ignore(Signal.Int);
        Check(Signal.Send(Process.Id(), Signal.Int) == 0, "signals: an ignored signal does nothing");
        Signal.Handle(Signal.Int, null);

        long child = Process.Start("usertest", "sleep");
        Process.Sleep(20);
        Check(Signal.Send((uint)child, Signal.Term) == 0 && Process.Wait((uint)child) == 128 + Signal.Term,
              "signals: the default action ends the program with 128 + the number");
        child = Process.Start("usertest", "sigterm");
        Process.Sleep(30);
        Signal.Send((uint)child, Signal.Term);
        Check(Process.Wait((uint)child) == 5, "signals: a program catches SIGTERM and finishes on its own terms");

        Signal.Handle(Signal.Pipe, &OnSignal);
        _signals = 0;
        int r = 0, w = 0;
        File.Pipe(out r, out w);
        File.Close(r);
        byte b = 1;
        Check(File.WriteBytes(w, &b, 1) == Process.ErrBrokenPipe && _signals == Signal.Pipe,
              "signals: SIGPIPE can be caught (the write fails, the program lives)");
        File.Close(w);
        Signal.Handle(Signal.Pipe, null);

        child = Process.Start("usertest", "sleep");
        Process.Sleep(20);
        ulong sender = Thread.Start(&SendLater, Process.Id());
        long first = Process.Wait((uint)child);
        Thread.Join(sender);
        Process.Kill((uint)child);
        Check(first == Process.ErrInterrupted && Process.Wait((uint)child) == Process.ExitKilled,
              "signals: a signal cuts wait() short, and the child can still be waited for");

        child = Process.Start("usertest", "sigspin");
        for (int i = 0; i < 50; i++) {
            Process.Sleep(2);
            Signal.Send((uint)child, Signal.Usr1);
        }
        Check(Process.Wait((uint)child) == 0, "signals: registers and FPU state survive 50 handlers in the middle of a computation");
    }

    static int SigSpin()
    {
        Signal.Handle(Signal.Usr1, &Clobber);
        ulong until = Time.UptimeMs() + 300;
        while (Time.UptimeMs() < until) {
            long a = 0, b = 1;
            double x = 0, y = 1.5;
            for (int i = 0; i < 100000; i++) {
                a += i ^ (int)b;
                b = b * 3 + 1;
                x += y * i;
                y = y * 0.999 + 0.001;
            }
            // The same computation again must give the same answer, whatever ran in between.
            long a2 = 0, b2 = 1;
            double x2 = 0, y2 = 1.5;
            for (int i = 0; i < 100000; i++) {
                a2 += i ^ (int)b2;
                b2 = b2 * 3 + 1;
                x2 += y2 * i;
                y2 = y2 * 0.999 + 0.001;
            }
            if (a != a2 || b != b2 || x != x2 || y != y2)
                return 1;
        }
        return _signals > 0 ? 0 : 2;
    }

    /// Uses integer and floating-point registers heavily, so a lost save would show.
    static void Clobber(int sig)
    {
        double d = sig;
        for (int i = 0; i < 64; i++)
            d = d * 1.7 + i;
        _signals += d > 0 ? 1 : 0;
    }

    static int IpcChild()
    {
        long fd = Channel.Connect("usertest-ipc");
        if (fd < 0)
            return 1;
        byte* buf = stackalloc byte[64];
        int shm = -1;
        if (Channel.Receive((int)fd, buf, 64, &shm) != 8 || shm < 0)
            return 2;
        long at = Device.Map(shm, 4096, Device.MapWrite);
        if (at < 0)
            return 3;
        byte* m = (byte*)at;
        m[0] = (byte)'p';
        m[1] = (byte)'o';
        m[2] = (byte)'n';
        m[3] = (byte)'g';
        Channel.Send((int)fd, Str.Bytes("done"), 4, -1);
        return 0;
    }

    static void Devices()
    {
        long fb = File.Open("/dev/fb0", File.Read);
        if (fb < 0)
            return;   // no framebuffer (host mode, serial-only machines)
        byte* info = stackalloc byte[64];
        Check(Device.Control((int)fb, 0x4401, info) == 0 && *(uint*)info > 0 && *(uint*)(info + 12) == 32,
              "ioctl(/dev/fb0, INFO): size and 32 bpp");
        Check(Device.Control((int)fb, 0x4402, info) == Process.ErrAccess, "presenting needs the display opened for writing");
        Check(Device.Control((int)fb, 0x7777, info) == Process.ErrNotSupported, "unknown ioctl requests are refused");
        File.Close((int)fb);
        long input = File.Open("/dev/input", File.Read);
        Check(input >= 0 && File.Open("/dev/input", File.Read) == Process.ErrBusy, "/dev/input has one reader at a time");
        File.Close((int)input);
        AudioDevice();
    }

    static void AudioDevice()
    {
        long fd = Audio.Open();
        if (fd < 0)
            return;   // no sound device
        long old = Audio.Volume((int)fd);
        Check(Audio.SetVolume((int)fd, 30) == 0 && Audio.Volume((int)fd) == 30 && Audio.SetVolume((int)fd, 101) == Process.ErrBadArgument,
              "audio: master volume set and read back, out of range refused");
        Audio.SetVolume((int)fd, (uint)old);
        Check(Device.Control((int)fd, 0x4103, null) == 48000, "audio: 48 kHz");
        int frames = 4800;   // 0.1 s of a quiet 440 Hz square
        short* pcm = (short*)Memory.Alloc((ulong)frames * 4);
        for (int i = 0; i < frames; i++)
            pcm[i * 2] = pcm[i * 2 + 1] = (short)((i / 54) % 2 == 0 ? 600 : -600);
        ulong start = Time.UptimeMs();
        long put = File.WriteBytes((int)fd, pcm, (ulong)frames * 4);
        long drained = Audio.Drain((int)fd);
        ulong took = Time.UptimeMs() - start;
        Check(put == frames * 4 && drained == 0 && Audio.Queued((int)fd) == 0 && took >= 80,
              "audio: 0.1 s of PCM queued, mixed and played (drain waits for it)");
        Memory.Free(pcm);
        File.Close((int)fd);
    }

    // ---------------------------------------------------------------- futex and pages

    static uint* _word;

    static void Waiter(ulong unused)
    {
        while (Atomic.Load((long*)_word) == 0)
            Futex.Wait(_word, 0);
        _word[1] = 7;   // proof it ran after the wake
    }

    static ulong MemoryKib()
    {
        ProcessInfo info;
        for (uint i = 0; Process.GetInfo(i, out info); i++)
            if (info.Pid == Process.Id())
                return info.MemoryKib;
        return 0;
    }

    static void DemandPaging()
    {
        ulong before = MemoryKib();
        byte* big = (byte*)Memory.MapPages(32 * 1024 * 1024);
        ulong reserved = MemoryKib();
        Check(big != null && reserved - before < 64, "demand paging: 32 MiB reserved cost (almost) nothing");
        bool zero = true;
        for (int i = 0; i < 32 * 1024 * 1024; i += 4096) {
            zero &= big[i] == 0;
            big[i] = 1;
        }
        ulong touched = MemoryKib();
        Check(zero && touched - before >= 32 * 1024 && touched - before < 33 * 1024, "demand paging: pages appear zeroed when first touched");
        long fd = File.Open("/proc/version", File.Read);
        byte* fresh = (byte*)Memory.MapPages(1 << 20);
        Check(fd >= 0 && File.ReadBytes((int)fd, fresh + 512 * 1024, 64) > 0 && fresh[512 * 1024] == 'N',
              "demand paging: the kernel can read() into pages nobody touched yet");
        File.Close((int)fd);
        Memory.UnmapPages(fresh, 1 << 20);
        Memory.UnmapPages(big, 32 * 1024 * 1024);
        Check(MemoryKib() - before < 64, "demand paging: unmap gives the touched pages back");
        Check(Deep(1500) == 1500, "the stack grows on demand (deep recursion, ~800 KiB)");
    }

    /// About 550 bytes of stack per level.
    static int Deep(int n)
    {
        byte* pad = stackalloc byte[512];
        pad[0] = (byte)n;
        if (n == 0)
            return pad[0];
        return Deep(n - 1) + 1 + pad[0] - (byte)n;
    }

    static void FutexAndPages()
    {
        _word = (uint*)Memory.AllocZeroed(16);
        Check(Futex.Wait(_word, 5) == Process.ErrBusy, "futex_wait returns at once when the value differs");
        Check(Futex.Wake(_word, 1) == 0, "futex_wake with nobody waiting wakes nobody");
        ulong t0 = Time.UptimeMs();
        Check(Futex.Wait(_word, 0, 20) == 0 && Time.UptimeMs() - t0 >= 20, "futex_wait with a timeout");
        ulong t = Thread.Start(&Waiter);
        Process.Sleep(20);   // let it go to sleep
        _word[0] = 1;
        long woken = Futex.Wake(_word, 1);
        Thread.Join(t);
        Check(woken == 1 && _word[1] == 7, "a sleeping thread is woken by futex_wake; Thread.Join sleeps too");

        ulong before = MemoryKib();
        byte* pages = (byte*)Memory.MapPages(256 * 1024);
        for (int i = 0; i < 256 * 1024; i += 4096)
            pages[i] = 1;
        ulong mapped = MemoryKib();
        Check(pages != null && mapped >= before + 256 && Memory.UnmapPages(pages, 256 * 1024) == 0 && MemoryKib() == before,
              "unmap gives pages back to the kernel");
        byte* again = (byte*)Memory.MapPages(64 * 1024);
        Check(again == pages && again[0] == 0, "an unmapped range is reused, zeroed");
        Memory.UnmapPages(again, 64 * 1024);
        Check(Process.Wait((uint)Process.Start("usertest", "unmapped")) == 142, "touching unmapped memory faults (142)");
        Check(Process.Wait((uint)Process.Start("usertest", "readonly")) == 142, "writing a read-only page faults, reading it works");
        Check(Memory.UnmapPages((void*)0x400000, 4096) == Process.ErrBadArgument, "unmap refuses the program image");
    }

    // ---------------------------------------------------------------- exceptions

    static int Fails(int v)
    {
        if (v > 2)
            throw new System.ArgumentOutOfRangeException("v", "too big");
        return v;
    }

    static long _threadResult;

    static void ThrowingThread(ulong arg)
    {
        long r = 0;
        try {
            Fails(5);
        } catch (System.ArgumentException e) {   // a base class of ArgumentOutOfRangeException
            r = Str.Equals(e.Message, "too big") ? 1 : -1;
        }
        _threadResult = r;   // Join orders it before the read
    }

    static void Exceptions()
    {
        int got = 0;
        try {
            got = Fails(1);
            got = Fails(3);
        } catch (System.ArgumentOutOfRangeException) {
            got += 100;
        }
        Check(got == 101, "try/catch in a program (main thread)");
        ulong t = Thread.Start(&ThrowingThread);
        Thread.Join(t);
        Check(_threadResult == 1, "try/catch in a second thread (its own TLS)");
        // Its "Unhandled exception" report goes to /dev/null: the boot self test is quiet.
        long quiet = File.Open("/dev/null", File.Write);
        long savedOut = File.Duplicate(1);
        long savedErr = File.Duplicate(2);
        File.Redirect((int)quiet, 1);
        File.Redirect((int)quiet, 2);
        long child = Process.Start("usertest", "uncaught");
        File.Redirect((int)savedOut, 1);
        File.Redirect((int)savedErr, 2);
        File.Close((int)quiet);
        File.Close((int)savedOut);
        File.Close((int)savedErr);
        Check(Process.Wait((uint)child) == 134, "an uncaught exception ends the program with 134");
    }

    // ---------------------------------------------------------------- sockets

    static int _listener;

    static void EchoServer(ulong unused)
    {
        long fd = Net.Accept(_listener, null);
        if (fd < 0)
            return;
        byte* buf = stackalloc byte[512];
        while (true)
        {
            long n = File.ReadBytes((int)fd, buf, 512);
            if (n <= 0)
                break;
            for (long i = 0; i < n; i++)
                if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] -= 32;
            Net.WriteAll((int)fd, buf, n);
        }
        File.Close((int)fd);
    }

    static void Sockets()
    {
        uint* config = stackalloc uint[4];
        if (Net.Config(config) < 0 || config[0] == 0)
            return;   // no network
        uint ip = 0;
        Check(Net.Resolve("localhost", out ip) == 0 && ip == 0x7F000001 && Net.Resolve("10.1.2.3", out ip) == 0 && ip == 0x0A010203,
              "resolve: localhost and dotted addresses");

        long a = Net.UdpOpen(0);
        long b = Net.UdpOpen(6000);
        Check(a >= 3 && b > a, "udp_open gives socket fds");
        byte* buf = stackalloc byte[64];
        uint* from = stackalloc uint[2];
        Check(Net.SendTo((int)a, 0x7F000001, 6000, Str.Bytes("ring3 udp"), 9) == 9
              && Net.ReceiveFrom((int)b, buf, 64, from, 1000) == 9 && buf[0] == 'r' && from[0] == 0x7F000001,
              "UDP between two sockets of a program");
        Check(Net.ReceiveFrom((int)b, buf, 64, from, 30) == 0, "recvfrom times out");
        File.Close((int)a);
        File.Close((int)b);

        long listener = Net.Listen(6001);
        _listener = (int)listener;
        ulong t = Thread.Start(&EchoServer);
        long c = Net.Connect(0x7F000001, 6001);
        Check(listener >= 0 && c >= 0, "listen + connect from ring 3");
        Check(Net.WriteAll((int)c, "hello, tcp") && Net.Shutdown((int)c) == 0, "write to a TCP socket, then shut down sending");
        long got = 0;
        while (true)
        {
            long n = File.ReadBytes((int)c, buf + got, (ulong)(64 - got));
            if (n <= 0)
                break;
            got += n;
        }
        buf[got] = 0;
        Thread.Join(t);
        Check(got == 10 && Str.Equals(buf, "HELLO, TCP"), "the answer arrives through read(), then end of stream");
        File.Close((int)c);
        File.Close((int)listener);
        Check(Net.Connect(0x7F000001, 6002) == Process.ErrNotFound, "connect to a closed port fails");
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
