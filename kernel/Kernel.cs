using Nekotia;
using Nekotia.Hal;
using Nekotia.OS.Drivers;

namespace Nekotia.OS;

public static unsafe class Kernel
{
    public const string Version = "0.2.0";

    /// Boot information from the runtime, valid for the whole lifetime of the kernel.
    public static BootInfo* Info;

    [KernelEntry]
    public static void Start(BootInfo* info)
    {
        Info = info;
        Banner();
        if (Str.StartsWith(info->CommandLine, "test")) {
            // `make test`: verbose self tests, then leave QEMU with the result.
            int failed = SelfTest.Run(true);
            Power.TestExit(failed == 0 ? (byte)0 : (byte)1);
        }
        SelfTest.Run(false);
        Keyboard.Init();
        Shell.Run();
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
