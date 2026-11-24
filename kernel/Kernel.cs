using Nekotia;
using Nekotia.Hal;
using Nekotia.IO;
using Nekotia.OS.Net;
using Nekotia.OS.Drivers;
using Nekotia.Processes;

namespace Nekotia.OS;

public static unsafe class Kernel
{
    public const string Version = "0.4.0";

    /// Boot information from the runtime, valid for the whole lifetime of the kernel.
    public static BootInfo* Info;

    [KernelEntry]
    public static void Start(BootInfo* info)
    {
        Info = info;
        Syscalls.Init();
        Banner();
        StartNetwork();   // before the self tests: they ping the gateway
        Proc.Init();
        if (Str.StartsWith(info->CommandLine, "test")) {
            // `make test`: verbose self tests, then leave QEMU with the result.
            int failed = SelfTest.Run(true);
            Power.TestExit(failed == 0 ? (byte)0 : (byte)1);
        }
        SelfTest.Run(false);
        MountDisk();
        Storage.Init();
        Motd();
        Rtc.Init();
        Keyboard.Init();
        Mouse.Init();
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
            // A brand-new disk (first sector all zeros, like the fresh build/disk.img) is formatted on the spot;
            // anything else might be someone's data and is left alone.
            byte* first = stackalloc byte[512];
            bool blank = Block.ReadBytes((uint)Block.Find(dev), 0, first, 512) == 512;
            for (int i = 0; i < 512; i++) {
                if (first[i] != 0)
                    blank = false;
            }
            if (!blank || Fat.Format(dev, Str.Bytes("NEKOTIA")) < 0) {
                Log.Info("disk: /dev/vda has no FAT32 file system — `mkfs vda` (erases it), then `mount vda /mnt`");
                return;
            }
            Log.Info("disk: /dev/vda was empty — formatted as FAT32");
            if (Fat.Mount(dev, Str.Bytes("/mnt")) == 0) {
                OpenFile* f = null;
                if (Vfs.Open("/mnt/readme.txt", Vfs.Write | Vfs.Create, out f) == 0) {
                    string text = "This file lives on build/disk.img (FAT32). Everything you put in /mnt survives a reboot.\n";
                    Vfs.WriteFile(f, Str.Bytes(text), Str.Length(text));
                    Vfs.Close(f);
                }
                Vfs.Unmount(Str.Bytes("/mnt"));
            }
        }
        long e = Fat.Mount(dev, Str.Bytes("/mnt"));
        if (e < 0) {
            Log.Error($"disk: cannot mount /dev/vda: {Process.ErrorText(e)}");
            return;
        }
        MountInfo m = default;
        Vfs.GetMount(0, out m);
        Log.Info($"disk: /dev/vda (FAT32) mounted on /mnt, {m.FreeBytes:size} free of {m.TotalBytes:size}");
    }

    /// The C# network stack on eth0, configured by DHCP (`make run` gives QEMU's user network: 10.0.2.x).
    static void StartNetwork()
    {
        Stack.Init();
        if (!Stack.Up)
            return;
        if (Dhcp.Configure()) {
            uint a = Stack.Address, g = Stack.Gateway;
            Log.Info($"net: eth0 {a >> 24}.{(a >> 16) & 255}.{(a >> 8) & 255}.{a & 255} via {g >> 24}.{(g >> 16) & 255}.{(g >> 8) & 255}.{g & 255} (DHCP)");
        } else {
            Log.Warn("net: eth0 is up, but no DHCP server answered (`dhcp` tries again)");
        }
    }

    /// /etc/motd from the initrd, if there is one.
    static void Motd()
    {
        OpenFile* f = null;
        if (Vfs.Open("/etc/motd", Vfs.Read, out f) < 0)
            return;
        byte* buffer = stackalloc byte[1025];
        long n = Vfs.ReadFile(f, buffer, 1024);
        Vfs.Close(f);
        if (n <= 0)
            return;
        buffer[n] = 0;
        Terminal.WriteLine();
        Terminal.SetColor(AnsiColor.Cyan);
        Terminal.Write(buffer);
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
