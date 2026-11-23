namespace Nekotia.Hal;

public static class Cpu
{
    /// <summary>Waits for the next interrupt (<c>hlt</c>). In host builds this ends the process.</summary>
    [Native("::neko::cpu::halt")]
    public static extern void Halt();

    /// <summary>Disables interrupts and halts forever.</summary>
    [Native("::neko::cpu::halt_forever")]
    [NoReturn]
    public static extern void HaltForever();

    [Native("::neko::cpu::disable_interrupts")]
    public static extern void DisableInterrupts();

    [Native("::neko::cpu::enable_interrupts")]
    public static extern void EnableInterrupts();

    /// <summary>Time-stamp counter (cycles since reset).</summary>
    [Native("::neko::cpu::read_tsc")]
    public static extern ulong ReadTimestamp();

    [Native("::neko::cpu::read_cr2")]
    public static extern ulong ReadCr2();

    [Native("::neko::cpu::read_cr3")]
    public static extern ulong ReadCr3();
}

/// <summary>x86 I/O ports.</summary>
public static class Port
{
    [Native("::neko::cpu::outb")]
    public static extern void Out8(ushort port, byte value);

    [Native("::neko::cpu::outw")]
    public static extern void Out16(ushort port, ushort value);

    [Native("::neko::cpu::outl")]
    public static extern void Out32(ushort port, uint value);

    [Native("::neko::cpu::inb")]
    public static extern byte In8(ushort port);

    [Native("::neko::cpu::inw")]
    public static extern ushort In16(ushort port);

    [Native("::neko::cpu::inl")]
    public static extern uint In32(ushort port);

    /// <summary>Tiny delay for slow legacy devices (write to port 0x80).</summary>
    [Native("::neko::cpu::io_wait")]
    public static extern void Wait();
}
