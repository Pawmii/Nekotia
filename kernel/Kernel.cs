using Nekotia;
using Nekotia.Hal;
using Nekotia.Mem;

namespace Nekotia.OS;

public static unsafe class Kernel
{
    public const string Version = "0.1.0";

    [KernelEntry]
    public static void Start(BootInfo* info)
    {
        Banner();

        Serial.Write("boot:   ");
        Serial.Write(info->BootloaderName);
        Serial.Write(" ");
        Serial.Write(info->BootloaderVersion);
        Serial.WriteLine();

        MemoryReport(info);
        SelfTest.Run();
        Graphics.DrawSplash(&info->Screen);

        Serial.WriteLine("kernel: idle (hlt loop)");
        while (true)
            Cpu.Halt();
    }

    static void Banner()
    {
        Serial.WriteLine();
        Serial.WriteLine("   /\\_/\\    Nekotia");
        Serial.Write("  ( o.o )   kernel ");
        Serial.WriteLine(Version);
        Serial.WriteLine("   > ^ <    written in C#, transpiled to C++ by nekoc");
        Serial.WriteLine();
    }

    static void MemoryReport(BootInfo* info)
    {
        Serial.Write("memory: ");
        Serial.WriteSize(info->UsableMemory);
        Serial.Write(" usable of ");
        Serial.WriteSize(info->TotalMemory);
        Serial.Write(" (");
        Serial.WriteDec(info->MemoryMapEntries);
        Serial.WriteLine(" map entries)");

        Serial.Write("heap:   ");
        Serial.WriteSize(Heap.FreeBytes());
        Serial.Write(" free of ");
        Serial.WriteSize(Heap.TotalBytes());
        Serial.WriteLine();

        Serial.Write("pages:  ");
        Serial.WriteDec(Pages.FreeCount());
        Serial.Write(" free 4 KiB frames (");
        Serial.WriteSize(Pages.FreeCount() * Pages.Size);
        Serial.WriteLine(")");
    }
}
