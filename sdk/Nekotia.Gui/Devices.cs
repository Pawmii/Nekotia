using Nekotia.User;

namespace Nekotia.Gui;

/// <summary>What /dev/fbN says about itself (<c>neko::display::Info</c>, ioctl 0x4401).</summary>
public unsafe struct ScreenInfo
{
    public uint Width, Height;
    public uint Pitch;      // bytes per row
    public uint Bpp;
    public uint Format;     // 1 = XRGB8888
    public uint Flags;      // 1 needs Present, 2 accelerated, 4 primary
    public ulong Size;      // bytes to map
    public fixed byte Driver[16];
    public uint RefreshHz;
    public uint Owner;
}

/// <summary>
/// A display for a display server: /dev/fbN opened for writing (which pauses the kernel console), its memory
/// mapped write-combining, and a back buffer in RAM to compose into. <see cref="Present"/> copies a rectangle
/// of the back buffer to the screen (and tells drivers that need it — virtio-gpu, a real GPU — to scan it out).
/// </summary>
public unsafe struct Screen
{
    public const ulong IoctlInfo = 0x4401, IoctlPresent = 0x4402;

    public int Fd;
    public ScreenInfo Info;
    public uint* Front;     // the device memory
    public Canvas Back;     // compose here
    public int Width, Height;

    /// <summary>Takes over display <paramref name="index"/>; a negative error when it is busy or missing.</summary>
    public static long Open(int index, out Screen screen)
    {
        screen = default;
        byte* path = stackalloc byte[16];
        Text t = new Text(path, 16);
        t.Write($"/dev/fb{index}");
        long fd = File.Open(path, File.Read | File.Write);
        if (fd < 0)
            return fd;
        screen.Fd = (int)fd;
        ScreenInfo info = default;
        long r = Device.Control(screen.Fd, IoctlInfo, &info);
        screen.Info = info;
        if (r < 0 || info.Bpp != 32) {
            File.Close(screen.Fd);
            return r < 0 ? r : Process.ErrNotSupported;
        }
        long at = Device.Map(screen.Fd, screen.Info.Size, Device.MapWrite);
        if (at < 0) {
            File.Close(screen.Fd);
            return at;
        }
        screen.Front = (uint*)at;
        screen.Width = (int)screen.Info.Width;
        screen.Height = (int)screen.Info.Height;
        screen.Back = Canvas.Create(screen.Width, screen.Height);
        if (screen.Back.Pixels == null) {
            screen.Close();
            return Process.ErrNoMemory;
        }
        return 0;
    }

    /// <summary>Back buffer → screen for <paramref name="r"/>.</summary>
    public void Present(Rect r)
    {
        Rect c = r.Intersect(new Rect(0, 0, Width, Height));
        if (c.IsEmpty)
            return;
        uint stride = Info.Pitch / 4;
        for (int y = c.Y; y < c.Bottom; y++)
            Memory.Copy(Front + (long)y * stride + c.X, Back.Row(y) + c.X, (ulong)c.W * 4);
        if ((Info.Flags & 1) != 0) {
            Rect copy = c;
            Device.Control(Fd, IoctlPresent, &copy);
        }
    }

    /// <summary>Gives the display back (the console repaints itself).</summary>
    public void Close()
    {
        if (Back.Pixels != null)
            Back.Free();
        if (Front != null)
            Device.Unmap(Front);
        Front = null;
        File.Close(Fd);
        Fd = -1;
    }
}

/// <summary>One event from /dev/input (<c>neko::input::Event</c>).</summary>
public struct InputEvent
{
    public const ushort Key = 1, Motion = 2, Button = 3, Wheel = 4, Absolute = 5;

    public ulong TimeMs;
    public ushort Type;
    public ushort Code;       // key: set 1 scancode (+0x100 after 0xE0); button: 0 left, 1 right, 2 middle
    public byte Pressed;
    public byte Mods;         // Keys.ModShift…
    public ushort Reserved;
    public uint Codepoint;    // the character of a key press (layout applied), 0 for none
    public int X, Y;          // motion: dx, dy; absolute: 0…32767; wheel: Y notches (positive = down)
    public uint Reserved2;
}

/// <summary>Keyboard and pointer for a display server: /dev/input, grabbed from the console while open.</summary>
public unsafe struct InputDevice
{
    public int Fd;

    public static long Open(out InputDevice input)
    {
        input = default;
        long fd = File.Open("/dev/input", File.Read);
        if (fd < 0)
            return fd;
        input.Fd = (int)fd;
        return 0;
    }

    /// <summary>Up to <paramref name="max"/> events (waits until there is one — poll Fd first to avoid that).</summary>
    public int Read(InputEvent* events, int max)
    {
        long n = File.ReadBytes(Fd, events, (ulong)max * (ulong)sizeof(InputEvent));
        return n < 0 ? 0 : (int)(n / sizeof(InputEvent));
    }

    public void Close() => File.Close(Fd);
}

/// <summary>Key codes (set 1 scancodes; +0x100 for E0-prefixed keys) and modifier bits of input events.</summary>
public static class Keys
{
    public const byte ModShift = 1, ModCtrl = 2, ModAlt = 4, ModCaps = 8, ModSuper = 16;

    public const ushort Escape = 0x01, Backspace = 0x0E, Tab = 0x0F, Enter = 0x1C, LeftCtrl = 0x1D, LeftShift = 0x2A,
        RightShift = 0x36, LeftAlt = 0x38, Space = 0x39, CapsLock = 0x3A,
        F1 = 0x3B, F2 = 0x3C, F3 = 0x3D, F4 = 0x3E, F5 = 0x3F, F10 = 0x44, F11 = 0x57, F12 = 0x58,
        Up = 0x148, Down = 0x150, Left = 0x14B, Right = 0x14D, Home = 0x147, End = 0x14F,
        PageUp = 0x149, PageDown = 0x151, Insert = 0x152, Delete = 0x153, RightCtrl = 0x11D, RightAlt = 0x138,
        LeftSuper = 0x15B, RightSuper = 0x15C;

    /// <summary>Codepoints the keyboard gives special keys (the console's Key enum).</summary>
    public const uint CharUp = 0x110000, CharDown = 0x110001, CharLeft = 0x110002, CharRight = 0x110003,
        CharHome = 0x110004, CharEnd = 0x110005, CharPageUp = 0x110006, CharPageDown = 0x110007,
        CharInsert = 0x110008, CharDelete = 0x110009;
}
