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
    public ulong Size;      // bytes to map (every buffer)
    public fixed byte Driver[16];
    public uint RefreshHz;
    public uint Owner;
    public uint Buffers;    // 2: double buffering (draw into the hidden one, then flip)
    public uint Shown;
    public ulong BufferOffset;
}

/// <summary>When the next vertical blank comes (<c>neko::display::Vblank</c>, ioctl 0x4404).</summary>
public struct VblankInfo
{
    public ulong Sequence;
    public ulong NextMs;      // uptime
    public uint PeriodUs;
    public uint Hardware;     // 0: the kernel's 60 Hz clock stands in for the scanout timing
}

/// <summary>
/// A display for a display server: /dev/fbN opened for writing (which pauses the kernel console), its memory
/// mapped write-combining, and a back buffer in RAM to compose into.
///
/// <see cref="PresentFrame"/> puts a finished frame on screen. With double buffering (a driver that can flip —
/// bochs on QEMU, real GPUs) it copies the changed rectangles into the hidden buffer and flips, so the screen
/// never shows a half-drawn frame; the hidden buffer is two frames old, so the previous frame's rectangles are
/// copied too. With one buffer it copies straight to the screen (and calls PRESENT for drivers that need it).
/// <see cref="NextVblankMs"/> paces frames to the refresh rate.
/// </summary>
public unsafe struct Screen
{
    public const ulong IoctlInfo = 0x4401, IoctlPresent = 0x4402, IoctlFlip = 0x4403, IoctlVblank = 0x4404,
        IoctlWaitVblank = 0x4405, IoctlPresentBuffer = 0x4406;

    public int Fd;
    public ScreenInfo Info;
    public uint* Front;     // the device memory (all buffers)
    public Canvas Back;     // compose here
    public int Width, Height;
    public int Hidden;      // buffer to draw into next (double buffering)
    public Damage Previous; // what the last frame changed (the hidden buffer lacks it)

    public bool DoubleBuffered => Info.Buffers >= 2;

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
        screen.Hidden = info.Shown == 0 ? 1 : 0;
        screen.Previous.Add(new Rect(0, 0, screen.Width, screen.Height));   // the hidden buffer starts out stale
        return 0;
    }

    /// <summary>Copies <paramref name="r"/> of the back buffer into buffer <paramref name="buffer"/>.</summary>
    void Copy(Rect r, int buffer)
    {
        Rect c = r.Intersect(new Rect(0, 0, Width, Height));
        if (c.IsEmpty)
            return;
        uint stride = Info.Pitch / 4;
        uint* page = (uint*)((byte*)Front + (long)buffer * (long)Info.BufferOffset);
        for (int y = c.Y; y < c.Bottom; y++)
            Memory.CopyStream(page + (long)y * stride + c.X, Back.Row(y) + c.X, (ulong)c.W * 4);
    }

    /// <summary>Shows the frame composed in Back; <paramref name="changed"/> is what differs from the last frame.</summary>
    public void PresentFrame(Damage* changed)
    {
        if (!DoubleBuffered) {
            for (int i = 0; i < changed->Count; i++)
                Present(changed->Get(i));
            return;
        }
        Damage all = Previous;
        for (int i = 0; i < changed->Count; i++)
            all.Add(changed->Get(i));
        bool needsPresent = (Info.Flags & 1) != 0;   // a GPU with memory of its own: hand it the changes
        int* args = stackalloc int[5];
        for (int i = 0; i < all.Count; i++) {
            Rect r = all.Get(i);
            Copy(r, Hidden);
            if (needsPresent) {
                args[0] = Hidden;
                args[1] = r.X;
                args[2] = r.Y;
                args[3] = r.W;
                args[4] = r.H;
                Device.Control(Fd, IoctlPresentBuffer, args);
            }
        }
        Device.Control(Fd, IoctlFlip, (void*)(ulong)Hidden);
        Hidden = 1 - Hidden;
        Previous = *changed;
    }

    /// <summary>Back buffer → screen for <paramref name="r"/> (single buffering; double buffering: use PresentFrame).</summary>
    public void Present(Rect r)
    {
        Rect c = r.Intersect(new Rect(0, 0, Width, Height));
        if (c.IsEmpty)
            return;
        Copy(c, (int)Info.Shown);
        if ((Info.Flags & 1) != 0) {
            Rect copy = c;
            Device.Control(Fd, IoctlPresent, &copy);
        }
    }

    /// <summary>Uptime (ms) of the next vertical blank: the moment to show the next frame.</summary>
    public ulong NextVblankMs()
    {
        VblankInfo v = default;
        if (Device.Control(Fd, IoctlVblank, &v) < 0)
            return Time.UptimeMs() + 16;
        return v.NextMs;
    }

    /// <summary>Sleeps until the next vertical blank.</summary>
    public void WaitVblank() => Device.Control(Fd, IoctlWaitVblank, null);

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
