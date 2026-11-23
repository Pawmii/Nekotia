namespace Nekotia.Hal;

/// <summary>Uptime and sleeping, driven by the 1 kHz local APIC timer.</summary>
public static class Time
{
    [Native("::neko::time::uptime_ms")]
    public static extern ulong UptimeMs();

    /// <summary>Halts the CPU until <paramref name="ms"/> milliseconds have passed (other interrupts keep running).</summary>
    [Native("::neko::time::sleep_ms")]
    public static extern void Sleep(ulong ms);

    [Native("::neko::time::timer_hz")]
    public static extern uint TimerHz();
}

/// <summary>Legacy ISA interrupt lines routed through the IOAPIC. Prefer the <c>[Irq(n)]</c> attribute.</summary>
public static unsafe class Irq
{
    [Native("::neko::irq::register_handler")]
    public static extern bool Register(byte line, delegate*<void> handler);

    /// <summary>How many times IRQ <paramref name="line"/> fired since boot.</summary>
    [Native("::neko::irq::count")]
    public static extern ulong Count(byte line);

    /// <summary>Message-signalled interrupt number <paramref name="index"/> (PCI MSI/MSI-X); false past the end.</summary>
    [Native("::neko::irq::msi_info")]
    public static extern bool GetMsi(uint index, out byte vector, out string name, out ulong count);
}

public static class Power
{
    [Native("::neko::power::reboot")]
    [NoReturn]
    public static extern void Reboot();

    /// <summary>Powers off in QEMU/Bochs/VirtualBox; on real hardware (no ACPI driver yet) it halts.</summary>
    [Native("::neko::power::shutdown")]
    [NoReturn]
    public static extern void Shutdown();

    /// <summary>Leaves QEMU with an exit status for automated tests (`make test`): 0 → status 1 (pass), other → failure.</summary>
    [Native("::neko::power::test_exit")]
    [NoReturn]
    public static extern void TestExit(byte code);
}

public static class SysInfo
{
    [Native("::neko::sysinfo::cpu_brand")]
    public static extern string CpuBrand();

    /// <summary>Enabled processors listed in the ACPI MADT (only the boot CPU runs code for now).</summary>
    [Native("::neko::sysinfo::cpu_count")]
    public static extern uint CpuCount();

    /// <summary>TSC frequency measured at boot.</summary>
    [Native("::neko::sysinfo::cpu_mhz")]
    public static extern ulong CpuMhz();

    [Native("::neko::sysinfo::apic_available")]
    public static extern bool HasApic();
}
