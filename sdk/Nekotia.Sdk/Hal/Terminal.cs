namespace Nekotia.Hal;

/// <summary>16 ANSI colours of the console palette.</summary>
public enum AnsiColor : byte
{
    Black, Red, Green, Yellow, Blue, Magenta, Cyan, White,
    Gray, BrightRed, BrightGreen, BrightYellow, BrightBlue, BrightMagenta, BrightCyan, BrightWhite,
}

/// <summary>
/// The kernel's main output: the framebuffer text console and the serial port at the same time.
/// UTF-8 strings (Cyrillic included) and ANSI escapes (colours, ESC[2J) work on both.
/// <c>Terminal.WriteLine($"{n} items")</c> is lowered by nekoc to a sequence of Write calls — no allocation.
/// </summary>
public static unsafe class Terminal
{
    [Native("::neko::term::write")]
    public static extern void Write(string text);

    [Native("::neko::term::write")]
    public static extern void Write(byte* cString);

    [Native("::neko::term::write_char")]
    public static extern void WriteChar(byte c);

    /// <summary>Writes a Unicode codepoint (encoded as UTF-8).</summary>
    [Native("::neko::term::write_codepoint")]
    public static extern void WriteCodepoint(uint codepoint);

    [Native("::neko::term::write_hex")]
    public static extern void WriteHex(ulong value);

    [Native("::neko::term::write_dec")]
    public static extern void WriteDec(long value);

    [Native("::neko::term::write_udec")]
    public static extern void WriteDec(ulong value);

    [Native("::neko::term::write_size")]
    public static extern void WriteSize(ulong bytes);

    /// <summary>Fixed notation with <paramref name="decimals"/> digits. In <c>$"..."</c>: <c>{x}</c> (3 digits) or <c>{x:F2}</c>.</summary>
    [Native("::neko::term::write_float")]
    public static extern void WriteFloat(double value, int decimals = 3);

    [Native("::neko::console::columns")]
    public static extern uint Columns();

    [Native("::neko::console::rows")]
    public static extern uint Rows();

    [Native("::neko::console::cursor_column")]
    public static extern uint CursorColumn();

    [Native("::neko::console::cursor_row")]
    public static extern uint CursorRow();

    [Native("::neko::console::set_cursor")]
    public static extern void SetCursor(uint column, uint row);

    [Native("::neko::console::set_cursor_visible")]
    public static extern void SetCursorVisible(bool visible);

    /// <summary>Repaints the console after something else (e.g. graphics) drew on the framebuffer.</summary>
    [Native("::neko::console::redraw")]
    public static extern void Redraw();

    [Native("::neko::console::ready")]
    public static extern bool HasScreen();

    // C# helpers.

    public static void WriteLine(string text)
    {
        Write(text);
        WriteChar((byte)'\n');
    }

    public static void WriteLine() => WriteChar((byte)'\n');

    public static void Clear() => Write("\x1b[2J\x1b[H");

    public static void SetColor(AnsiColor color)
    {
        int code = color < AnsiColor.Gray ? 30 + (int)color : 90 + (int)color - 8;
        Write("\x1b[");
        WriteDec(code);
        WriteChar((byte)'m');
    }

    public static void ResetColor() => Write("\x1b[0m");

    /// <summary>Writes <paramref name="text"/> in <paramref name="color"/> and restores the default colour.</summary>
    public static void Write(AnsiColor color, string text)
    {
        SetColor(color);
        Write(text);
        ResetColor();
    }

    public static void WriteLine(AnsiColor color, string text)
    {
        Write(color, text);
        WriteLine();
    }
}
