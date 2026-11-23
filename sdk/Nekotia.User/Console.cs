namespace Nekotia.User;

/// <summary>
/// Text output and keyboard input of a user program (runtime/user/src/user.cpp → write/read system calls).
/// Output is buffered and flushed at every '\n', before reading and at exit. Supports string interpolation:
/// <c>Console.WriteLine($"pid {Process.Id()}, {bytes:size}")</c>. The kernel terminal understands ANSI colors (<see cref="Ansi"/>).
/// </summary>
public static unsafe class Console
{
    [Native("::neko::user::write")]
    public static extern void Write(string text);

    [Native("::neko::user::write")]
    public static extern void Write(byte* cString);

    [Native("::neko::user::write_char")]
    public static extern void WriteChar(byte c);

    /// <summary>Unicode codepoint, encoded as UTF-8.</summary>
    [Native("::neko::user::write_codepoint")]
    public static extern void WriteCodepoint(uint codepoint);

    [Native("::neko::user::write_hex")]
    public static extern void WriteHex(ulong value);

    [Native("::neko::user::write_dec")]
    public static extern void WriteDec(long value);

    [Native("::neko::user::write_udec")]
    public static extern void WriteDec(ulong value);

    /// <summary>Human-readable size: 512 B, 64 KiB, 1.5 MiB…</summary>
    [Native("::neko::user::write_size")]
    public static extern void WriteSize(ulong bytes);

    [Native("::neko::user::write_float")]
    public static extern void WriteFloat(double value, int decimals = 3);

    /// <summary>Unbuffered, to stderr.</summary>
    [Native("::neko::user::write_error")]
    public static extern void WriteError(string text);

    public static void WriteLine(string text)
    {
        Write(text);
        WriteChar((byte)'\n');
    }

    public static void WriteLine() => WriteChar((byte)'\n');

    public static void Write(string color, string text)
    {
        Write(color);
        Write(text);
        Write(Ansi.Reset);
    }

    public static void WriteLine(string color, string text)
    {
        Write(color, text);
        WriteLine();
    }

    /// <summary>
    /// Reads from the keyboard: the kernel returns whole lines (with '\n') once Enter is pressed.
    /// 0 = end of input (Ctrl+D). Ctrl+C ends the program.
    /// </summary>
    [Native("::neko::user::read")]
    public static extern long Read(byte* buffer, ulong length);

    /// <summary>One line without the '\n', NUL-terminated in <paramref name="buffer"/>; its length, or -1 at end of input.</summary>
    [Native("::neko::user::read_line")]
    public static extern long ReadLine(byte* buffer, ulong capacity);
}

/// <summary>ANSI escape sequences for <see cref="Console.Write(string, string)"/>.</summary>
public static class Ansi
{
    public const string Reset = "\x1b[0m";
    public const string Red = "\x1b[91m";
    public const string Green = "\x1b[92m";
    public const string Yellow = "\x1b[93m";
    public const string Blue = "\x1b[94m";
    public const string Magenta = "\x1b[95m";
    public const string Cyan = "\x1b[96m";
    public const string Gray = "\x1b[90m";
}
