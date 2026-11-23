namespace Nekotia.Hal;

/// <summary>Mirrors <c>neko::log::Level</c>.</summary>
[Native("::neko::log::Level")]
public enum LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 }

/// <summary>
/// The kernel log (runtime/src/core/log.cpp): leveled, timestamped messages in a 64 KiB ring (<c>dmesg</c>,
/// <c>/proc/log</c>). Messages at or above <see cref="Level"/> (kernel command line <c>loglevel=</c>, default
/// info) also go to the screen; the others only to serial. Interpolation works: <c>Log.Info($"net: {n} packets")</c>.
/// </summary>
public static unsafe class Log
{
    [Native("::neko::log::debug")] public static extern void Debug(string text);
    [Native("::neko::log::debug")] public static extern void Debug(Interpolated text);

    [Native("::neko::log::info")] public static extern void Info(string text);
    [Native("::neko::log::info")] public static extern void Info(Interpolated text);

    [Native("::neko::log::warn")] public static extern void Warn(string text);
    [Native("::neko::log::warn")] public static extern void Warn(Interpolated text);

    [Native("::neko::log::error")] public static extern void Error(string text);
    [Native("::neko::log::error")] public static extern void Error(Interpolated text);

    /// <summary>Messages below this level go to serial only.</summary>
    [Native("::neko::log::level")] public static extern LogLevel Level();
    [Native("::neko::log::set_level")] public static extern void SetLevel(LogLevel level);

    /// <summary>Ends the boot transcript (the shell calls it once it is up).</summary>
    [Native("::neko::log::stop_capture")] public static extern void StopBootCapture();

    [Native("::neko::log::size")] public static extern ulong Size();

    /// <summary>Copies bytes of the ring from <paramref name="offset"/> (oldest first); how many.</summary>
    [Native("::neko::log::read")] public static extern long Read(ulong offset, byte* buffer, ulong length);
}
