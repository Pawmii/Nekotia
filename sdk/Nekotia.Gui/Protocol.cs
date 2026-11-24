namespace Nekotia.Gui;

/// <summary>
/// The Nekotia window protocol: what applications and a display server (window manager + compositor) say to
/// each other over a channel (<see cref="Nekotia.User.Channel"/>). Any program that listens under
/// <see cref="Protocol.ServiceName"/> and speaks this is a desktop: nekowm is one, a tiling WM or a kiosk
/// shell would be others — applications do not change.
///
/// Every message is one <see cref="Message"/>. A window's pixels live in shared memory the client creates
/// (Width × Height × 4 bytes per buffer, XRGB8888, rows without padding) and attaches to CreateWindow / Attach.
/// With FlagDoubleBuffered there are two buffers back to back: the client draws into the one the server does
/// not show, sends Damage with its index (Value), and the server switches to it at once and answers Release
/// for the other — from then on the client may draw there. The server never reads a buffer being drawn into.
/// After composing, the server sends Frame, so animations can pace themselves.
///
/// Client → server: Hello, CreateWindow (+ shm), Attach (+ shm, new size), Damage, SetTitle, Destroy, Move.
/// Server → client: Welcome, Configure (please use this size), Close (the user asked), Focus, Key, Pointer, Frame,
/// Release.
/// </summary>
public static class Protocol
{
    public const string ServiceName = "display";
    public const uint Version = 1;

    // client → server
    public const uint Hello = 1;          // Value = version
    public const uint CreateWindow = 2;   // Window = client's id, Width/Height, Flags, Text = title; attached: shm
    public const uint Attach = 3;         // Window, Width/Height of the new buffer; attached: shm
    public const uint Damage = 4;         // Window, X/Y/Width/Height (window coordinates), Value = buffer (double)
    public const uint SetTitle = 5;       // Window, Text
    public const uint Destroy = 6;        // Window
    public const uint Move = 7;           // Window, X/Y — a request; the server decides

    // server → client
    public const uint Welcome = 101;      // Width/Height = screen size, Value = version
    public const uint Configure = 102;    // Window, Width/Height: the size the server wants (maximise, resize)
    public const uint Close = 103;        // Window: the user closed it (the client destroys it, or not)
    public const uint Focus = 104;        // Window, Value = 1 focused / 0 not
    public const uint Key = 105;          // Window, Code, Value = pressed, Codepoint, Mods
    public const uint Pointer = 106;      // Window, Code = PointerMove/Button/Wheel/Enter/Leave, X/Y, Value, Mods
    public const uint Frame = 107;        // Window: the last Damage is on screen
    public const uint Release = 108;      // Window, Value = buffer the server no longer reads (double buffering)

    // Pointer kinds (Message.Code of Pointer)
    public const uint PointerMove = 0, PointerButton = 1, PointerWheel = 2, PointerEnter = 3, PointerLeave = 4;

    // CreateWindow flags
    public const uint FlagFixedSize = 1;   // no resizing by the user
    public const uint FlagNoDecorations = 2;
    public const uint FlagCentered = 4;
    public const uint FlagDoubleBuffered = 8;   // two buffers in the shared memory (App always uses it)
}

/// <summary>One protocol message (fixed size; unused fields are 0).</summary>
public unsafe struct Message
{
    public uint Type;
    public uint Window;
    public int X, Y, Width, Height;
    public uint Code;
    public uint Value;
    public uint Codepoint;
    public uint Mods;
    public uint Flags;
    public uint Reserved;
    public fixed byte Text[64];

    public void SetText(byte* s)
    {
        int i = 0;
        for (; s != null && s[i] != 0 && i < 63; i++)
            Text[i] = s[i];
        if (s != null && s[i] != 0) {
            // cut: drop the last (possibly incomplete) UTF-8 sequence
            while (i > 0 && (Text[i - 1] & 0xC0) == 0x80)
                i--;
            if (i > 0 && Text[i - 1] >= 0xC0)
                i--;
        }
        Text[i] = 0;
    }
}
