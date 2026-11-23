namespace Nekotia.Hal;

/// <summary>COM1 serial port (115200 8N1). In QEMU it is wired to the terminal (-serial stdio); in host builds to stdout.</summary>
public static unsafe class Serial
{
    [Native("::neko::serial::write")]
    public static extern void Write(string text);

    [Native("::neko::serial::write")]
    public static extern void Write(byte* cString);

    [Native("::neko::serial::write")]
    public static extern void Write(Interpolated text);

    [Native("::neko::serial::write")]
    public static extern void WriteLine(Interpolated text);

    [Native("::neko::serial::write_char")]
    public static extern void WriteChar(byte c);

    /// <summary>Unsigned hexadecimal with a 0x prefix.</summary>
    [Native("::neko::serial::write_hex")]
    public static extern void WriteHex(ulong value);

    [Native("::neko::serial::write_dec")]
    public static extern void WriteDec(long value);

    [Native("::neko::serial::write_udec")]
    public static extern void WriteDec(ulong value);

    /// <summary>Human-readable size: 512 B, 64 KiB, 255 MiB…</summary>
    [Native("::neko::serial::write_size")]
    public static extern void WriteSize(ulong bytes);

    // Plain C# helpers: compiled to C++ by nekoc like any kernel code.

    public static void WriteLine(string text)
    {
        Write(text);
        WriteChar((byte)'\n');
    }

    public static void WriteLine() => WriteChar((byte)'\n');

    public static void WriteField(string label, ulong value)
    {
        Write(label);
        Write(": ");
        WriteDec(value);
        WriteLine();
    }
}
