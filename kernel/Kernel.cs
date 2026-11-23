using Nekotia;
using Nekotia.Hal;
using Nekotia.IO;
using Nekotia.OS.Drivers;
using Nekotia.Processes;

namespace Nekotia.OS;

public static unsafe class Kernel
{
    public const string Version = "0.3.0";

    /// Boot information from the runtime, valid for the whole lifetime of the kernel.
    public static BootInfo* Info;

    [KernelEntry]
    public static void Start(BootInfo* info)
    {
        Info = info;
        Syscalls.Init();
        Banner();
        if (Str.StartsWith(info->CommandLine, "test")) {
            // `make test`: verbose self tests, then leave QEMU with the result.
            int failed = SelfTest.Run(true);
            Power.TestExit(failed == 0 ? (byte)0 : (byte)1);
        }
        SelfTest.Run(false);
        MountDisk();
        Keyboard.Init();
        RunInit((byte*)info->CommandLine);
        Shell.Run();
    }

    /// `init=NAME` on the kernel command line (boot/limine.conf): run /bin/NAME (or a path) in the foreground first —
    /// e.g. `init=sh` boots into the user-mode shell. When it exits, the kernel shell takes over.
    static void RunInit(byte* cmdline)
    {
        byte* name = stackalloc byte[128];
        int n = 0;
        for (byte* p = cmdline; *p != 0; p++) {
            bool tokenStart = p == cmdline || p[-1] == ' ';
            if (!(tokenStart && Str.StartsWith(p, "init=")))
                continue;
            p += 5;
            while (*p != 0 && *p != ' ' && n < 127)
                name[n++] = *p++;
            break;
        }
        name[n] = 0;
        if (n == 0)
            return;

        long pid = Process.Start(name, (byte*)null);
        if (pid < 0) {
            Terminal.Write(AnsiColor.BrightRed, "init: ");
            Terminal.Write(name);
            Terminal.Write(": ");
            Terminal.WriteLine(Process.ErrorText(pid));
            return;
        }
        long code = Process.Wait((uint)pid);
        Terminal.SetColor(AnsiColor.Gray);
        Terminal.Write("[init ");
        Terminal.Write(name);
        Terminal.Write($" exited with code {code} — kernel shell]");
        Terminal.ResetColor();
        Terminal.WriteLine();
    }

    /// The first virtio disk goes to /mnt when it holds FAT32 (`make run` attaches build/disk.img as vda).
    static void MountDisk()
    {
        byte* dev = Str.Bytes("vda");
        if (Block.Find(dev) < 0)
            return;
        if (!Fat.Probe(dev)) {
            Terminal.SetColor(AnsiColor.Gray);
            Terminal.WriteLine("disk: /dev/vda has no FAT32 file system — `mkfs vda`, then `mount vda /mnt`");
            Terminal.ResetColor();
            return;
        }
        long e = Fat.Mount(dev, Str.Bytes("/mnt"));
        if (e < 0) {
            Terminal.Write(AnsiColor.BrightRed, "disk: cannot mount /dev/vda: ");
            Terminal.WriteLine(Process.ErrorText(e));
            return;
        }
        MountInfo m = default;
        Vfs.GetMount(0, out m);
        Terminal.SetColor(AnsiColor.Gray);
        Terminal.WriteLine($"disk: /dev/vda (FAT32) mounted on /mnt, {m.FreeBytes:size} free of {m.TotalBytes:size}");
        Terminal.ResetColor();
    }

    static void Banner()
    {
        Terminal.WriteLine();
        Terminal.SetColor(AnsiColor.BrightMagenta);
        Terminal.WriteLine("   /\\_/\\   ");
        Terminal.Write("  ( o.o )   ");
        Terminal.SetColor(AnsiColor.BrightWhite);
        Terminal.WriteLine($"Nekotia {Version}");
        Terminal.SetColor(AnsiColor.BrightMagenta);
        Terminal.Write("   > ^ <    ");
        Terminal.ResetColor();
        Terminal.WriteLine("kernel in C# → C++ (nekoc) → x86_64");
        Terminal.WriteLine();
    }
}
