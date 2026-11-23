using System.Runtime.CompilerServices;

namespace Nekotia;

public static class Debug
{
    /// <summary>
    /// Stops the kernel with the KERNEL_PANIC stop screen. The caller's file, line and method are filled in by the compiler.
    /// </summary>
    [Native("::neko::crash::panic_at")]
    [NoReturn]
    public static extern void Panic(string message,
        [CallerFilePath] string file = "", [CallerLineNumber] int line = 0, [CallerMemberName] string member = "");

    /// <summary>Shows the ASSERTION_FAILED stop screen when <paramref name="condition"/> is false.</summary>
    public static void Assert(bool condition, string message,
        [CallerFilePath] string file = "", [CallerLineNumber] int line = 0, [CallerMemberName] string member = "")
    {
        if (!condition)
            AssertFailed(message, file, line, member);
    }

    [Native("::neko::crash::assert_failed")]
    [NoReturn]
    static extern void AssertFailed(string message, string file, int line, string member);
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

    [Native("::neko::str::length")]
    public static extern nuint Length(byte* s);

    [Native("::neko::str::starts_with")]
    public static extern bool StartsWith(byte* s, string prefix);
}
