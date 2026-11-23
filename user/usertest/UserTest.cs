// Checks of the user-mode ABI, run by the kernel self test (kernel/SelfTest.neko):
//   usertest [-v] ARGS…   system calls, arguments, heap, FPU, child processes → exit code 42 when all pass
//   usertest child        exits with 7 (for spawn/wait)
//   usertest pf | gp | ud | kernel   faults on purpose; the kernel must kill only this process
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
            _verbose = Str.Equals(mode, "-v");
        }

        Arguments(argc, argv);
        SystemCalls();
        BadPointers();
        HeapAndFpu();
        Children();

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
}
