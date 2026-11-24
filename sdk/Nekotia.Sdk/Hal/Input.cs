namespace Nekotia.Hal;

/// <summary>Mirrors <c>neko::input::Stats</c>.</summary>
[Native("::neko::input::Stats")]
public struct InputStats
{
    [Native("events")] public ulong Events;
    [Native("dropped")] public ulong Dropped;
    /// <summary>pid of the program reading /dev/input, 0 = nobody (keys go to the console).</summary>
    [Native("grabbed_by")] public uint GrabbedBy;
}

/// <summary>Mirrors <c>neko::display::Info</c>: a display (/dev/fbN) and its driver.</summary>
[Native("::neko::display::Info")]
public unsafe struct DisplayInfo
{
    [Native("width")] public uint Width;
    [Native("height")] public uint Height;
    [Native("pitch")] public uint Pitch;
    [Native("bpp")] public uint Bpp;
    [Native("format")] public uint Format;
    [Native("flags")] public uint Flags;
    [Native("size")] public ulong Size;
    [Native("driver")] public fixed byte Driver[16];
    [Native("refresh_hz")] public uint RefreshHz;
    /// <summary>pid of the display server drawing on it, 0 = the console.</summary>
    [Native("owner")] public uint Owner;
    /// <summary>2 when the driver can flip between two buffers.</summary>
    [Native("buffers")] public uint Buffers;
    [Native("shown")] public uint Shown;
    [Native("buffer_offset")] public ulong BufferOffset;
}

/// <summary>
/// The input event stream (runtime/include/neko/input.hpp). Drivers call these from interrupt handlers;
/// while a display server reads /dev/input it gets every event, otherwise keys go to the console.
/// </summary>
public static class Input
{
    public const byte ModShift = 1, ModCtrl = 2, ModAlt = 4, ModCaps = 8, ModSuper = 16;

    [Native("::neko::input::grabbed")]
    public static extern bool Grabbed();

    /// <summary>Key <paramref name="code"/> (set 1 scancode, +0x100 behind 0xE0) went down or up.</summary>
    [Native("::neko::input::key")]
    public static extern void Key(ushort code, bool pressed, byte mods, uint codepoint);

    [Native("::neko::input::motion")]
    public static extern void Motion(int dx, int dy);

    /// <summary>Button state of one device (bit 0 left, 1 right, 2 middle); <paramref name="last"/> is that device's
    /// previous state, updated here.</summary>
    [Native("::neko::input::buttons")]
    public static extern void Buttons(ref byte last, byte mask);

    [Native("::neko::input::wheel")]
    public static extern void Wheel(int notches);

    /// <summary>Ctrl+Alt+Backspace: ends the display server (the grabbing program and the screen's owner).</summary>
    [Native("::neko::input::zap")]
    public static extern void Zap();

    [Native("::neko::input::stats")]
    public static extern InputStats Stats();
}

/// <summary>Displays registered by graphics drivers (runtime/include/neko/display.hpp).</summary>
public static class Displays
{
    [Native("::neko::display::count")]
    public static extern uint Count();

    [Native("::neko::display::info")]
    public static extern bool GetInfo(uint index, out DisplayInfo info);
}
