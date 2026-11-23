namespace Nekotia;

/// <summary>
/// Builds text in a buffer you own (stack, heap, a generated /proc file…) — interpolation included:
/// <code>
/// byte* buffer = stackalloc byte[128];
/// var t = new Text(buffer, 128);
/// t.WriteLine($"{name,-12}{size,10:size}");
/// </code>
/// It never writes past the capacity (the rest is dropped) and keeps the text NUL-terminated.
/// </summary>
public unsafe struct Text
{
    public byte* Data;
    public int Length;
    public int Capacity;   // including the NUL

    public Text(byte* buffer, ulong capacity)
    {
        Data = buffer;
        Length = 0;
        Capacity = (int)capacity;
        if (Capacity > 0)
            Data[0] = 0;
    }

    public void WriteChar(byte c)
    {
        if (Length + 1 >= Capacity)
            return;
        Data[Length++] = c;
        Data[Length] = 0;
    }

    public void Write(byte* s)
    {
        for (; s != null && *s != 0; s++)
            WriteChar(*s);
    }

    public void Write(string s) => Write(Str.Bytes(s));

    public void WriteLine() => WriteChar((byte)'\n');

    public void WriteLine(string s)
    {
        Write(s);
        WriteLine();
    }

    public void WriteDec(long v)
    {
        byte* digits = stackalloc byte[24];
        Format.Dec(digits, v);
        Write(digits);
    }

    public void WriteDec(ulong v)
    {
        byte* digits = stackalloc byte[24];
        Format.UDec(digits, v);
        Write(digits);
    }

    public void WriteHex(ulong v)
    {
        byte* digits = stackalloc byte[24];
        Format.Hex(digits, v);
        Write(digits);
    }

    public void WriteSize(ulong bytes)
    {
        byte* digits = stackalloc byte[24];
        Format.Size(digits, bytes);
        Write(digits);
    }

    public void WriteFloat(double v, int decimals = 3)
    {
        byte* digits = stackalloc byte[40];
        Format.Fixed(digits, v, decimals);
        Write(digits);
    }

    /// <summary>Lowered by nekoc into the calls above.</summary>
    [Native("Write")]
    public extern void Write(Interpolated text);

    [Native("WriteLine")]
    public extern void WriteLine(Interpolated text);
}

/// <summary>Number → text into a buffer (runtime/include/neko/fmt.hpp); each returns the length written (NUL-terminated).</summary>
public static unsafe class Format
{
    [Native("::neko::fmt::dec")]
    public static extern int Dec(byte* output, long value);

    [Native("::neko::fmt::udec")]
    public static extern int UDec(byte* output, ulong value);

    /// <summary>0x… without leading zeros.</summary>
    [Native("::neko::fmt::hex")]
    public static extern int Hex(byte* output, ulong value);

    /// <summary>Exactly <paramref name="digits"/> hex digits, no prefix.</summary>
    [Native("::neko::fmt::hex_digits")]
    public static extern int HexDigits(byte* output, ulong value, int digits);

    /// <summary>512 B, 64 KiB, 1.5 MiB…</summary>
    [Native("::neko::fmt::size")]
    public static extern int Size(byte* output, ulong bytes);

    [Native("::neko::fmt::fixed")]
    public static extern int Fixed(byte* output, double value, int decimals);

    /// <summary>Characters (not bytes) of a UTF-8 string.</summary>
    [Native("::neko::fmt::width")]
    public static extern int Width(byte* text);
}
