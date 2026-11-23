namespace Nekotia;

public static class Debug
{
    /// <summary>Prints the message to the serial port and stops the machine for good.</summary>
    [Native("::neko::panic")]
    [NoReturn]
    public static extern void Panic(string message);

    /// <summary>Panics when <paramref name="condition"/> is false.</summary>
    public static void Assert(bool condition, string message)
    {
        if (!condition)
            Panic(message);
    }
}

/// <summary>Helpers for NUL-terminated C strings (the kernel's <c>string</c> is a UTF-8 <c>const char*</c>).</summary>
public static unsafe class Str
{
    [Native("::neko::str::length")]
    public static extern nuint Length(string s);

    [Native("::neko::str::equals")]
    public static extern bool Equals(string a, string b);

    [Native("::neko::str::equals")]
    public static extern bool Equals(byte* a, string b);
}
