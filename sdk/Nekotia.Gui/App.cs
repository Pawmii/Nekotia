using Nekotia.User;

namespace Nekotia.Gui;

/// <summary>
/// A window of this program. Its pixels live in shared memory with the display server — two buffers of
/// Width × Height: the server shows one, <see cref="Canvas"/> is the other. App.Present hands the canvas over
/// and gives back the other buffer with the same picture, so drawing simply continues where it was.
/// </summary>
public unsafe struct Window
{
    public uint Id;
    public int Width, Height;
    public Canvas Canvas;     // draw here, then App.Present / App.Damage
    public bool Open;
    public bool Focused;
    public ulong BufferBytes; // one buffer
    public byte* Base;        // the mapping: buffer 0, then buffer 1
    public int Back;          // the buffer Canvas draws into
    public int PendingAttach; // fd of a new buffer pair to send with the next Present (after Resize), else -1
}

/// <summary>
/// The connection of an application to the display server (whichever desktop runs — see <see cref="Protocol"/>).
/// <code>
/// App.Connect();
/// App.CreateWindow("Hello", 320, 200, 0, out Window w);
/// w.Canvas.Clear(Theme.Window); App.Present(ref w);
/// while (App.Next(out Message m, -1) > 0) { … }
/// </code>
/// </summary>
public static unsafe class App
{
    const int PendingMax = 64;

    static int _fd = -1;
    static int _screenWidth, _screenHeight;
    static uint _nextId = 1;
    static Font _font;
    static Message* _pending;    // messages that arrived while Present waited for a Release
    static int _pendingHead, _pendingCount;

    /// <summary>The channel to the server (to poll it together with other fds — check HasPending too).</summary>
    public static int Fd => _fd;
    public static int ScreenWidth => _screenWidth;
    public static int ScreenHeight => _screenHeight;

    /// <summary>Messages already received and waiting for Next (poll would not show them).</summary>
    public static bool HasPending => _pendingCount > 0;

    /// <summary>The system font (Terminus 8×16), loaded by Connect.</summary>
    public static Font* SystemFont
    {
        get
        {
            fixed (Font* f = &_font) {
                return f;
            }
        }
    }

    /// <summary>Connects to the display server: 0, or a negative error (ErrNotFound: no desktop is running).</summary>
    public static long Connect()
    {
        long fd = Channel.Connect(Protocol.ServiceName);
        if (fd < 0)
            return fd;
        _fd = (int)fd;
        _pending = Memory.NewArray<Message>(PendingMax);
        Message m = default;
        m.Type = Protocol.Hello;
        m.Value = Protocol.Version;
        Send(&m);
        // Wait for Welcome (nothing else comes before it).
        while (true) {
            if (Receive(&m) <= 0) {
                File.Close(_fd);
                _fd = -1;
                return Process.ErrBrokenPipe;
            }
            if (m.Type == Protocol.Welcome)
                break;
        }
        _screenWidth = m.Width;
        _screenHeight = m.Height;
        Font.Load(Font.DefaultPath, out _font);
        return 0;
    }

    static long Send(Message* m, int attachFd = -1)
    {
        // The server drains its queue quickly; when it is full, wait a little instead of losing the message.
        for (int tries = 0; tries < 200; tries++) {
            long r = Channel.Send(_fd, m, (ulong)sizeof(Message), attachFd);
            if (r != Process.ErrBusy)
                return r;
            Process.Sleep(2);
        }
        return Process.ErrBusy;
    }

    /// <summary>One message from the channel (waits); files that come along are not expected and closed.</summary>
    static long Receive(Message* m)
    {
        int fdOut = -1;
        long n = Channel.Receive(_fd, m, (ulong)sizeof(Message), &fdOut);
        if (fdOut >= 0)
            File.Close(fdOut);
        return n;
    }

    static void Keep(Message* m)
    {
        if (_pendingCount == PendingMax) {   // full: the oldest goes
            _pendingHead = (_pendingHead + 1) % PendingMax;
            _pendingCount--;
        }
        _pending[(_pendingHead + _pendingCount) % PendingMax] = *m;
        _pendingCount++;
    }

    /// <summary>Two buffers of width × height pixels, mapped here: the fd (to attach) and the address.</summary>
    static long NewBuffers(int width, int height, out byte* pixels, out ulong bytes)
    {
        pixels = null;
        bytes = (ulong)width * (ulong)height * 4;
        long shm = SharedMemory.Create(bytes * 2);
        if (shm < 0)
            return shm;
        long at = Device.Map((int)shm, bytes * 2, Device.MapWrite);
        if (at < 0) {
            File.Close((int)shm);
            return at;
        }
        pixels = (byte*)at;
        return shm;
    }

    static void PointCanvas(ref Window w) =>
        w.Canvas = new Canvas((uint*)(w.Base + (long)w.Back * (long)w.BufferBytes), w.Width, w.Height, w.Width);

    /// <summary>
    /// Opens a window (title UTF-8, size of its content without decorations). Flags: Protocol.Flag*.
    /// The content starts black: draw, then Present.
    /// </summary>
    public static long CreateWindow(byte* title, int width, int height, uint flags, out Window window)
    {
        window = default;
        window.PendingAttach = -1;
        if (_fd < 0)
            return Process.ErrBadFd;
        if (width < 1 || height < 1 || width > 4096 || height > 4096)
            return Process.ErrBadArgument;
        byte* pixels = null;
        ulong bytes = 0;
        long shm = NewBuffers(width, height, out pixels, out bytes);
        if (shm < 0)
            return shm;
        window.Id = _nextId++;
        window.Width = width;
        window.Height = height;
        window.Base = pixels;
        window.BufferBytes = bytes;
        window.Back = 0;
        window.Open = true;
        PointCanvas(ref window);

        Message m = default;
        m.Type = Protocol.CreateWindow;
        m.Window = window.Id;
        m.Width = width;
        m.Height = height;
        m.Flags = flags | Protocol.FlagDoubleBuffered;
        m.SetText(title);
        long r = Send(&m, (int)shm);
        File.Close((int)shm);   // the mapping (and the server's copy) keep the memory
        return r < 0 ? r : 0;
    }

    public static long CreateWindow(string title, int width, int height, uint flags, out Window window) =>
        CreateWindow(Str.Bytes(title), width, height, flags, out window);

    /// <summary>
    /// The canvas is finished for <paramref name="r"/>: the server shows this buffer from now on, and the canvas
    /// becomes the other one with the same picture (waits briefly until the server lets go of it).
    /// </summary>
    public static void Damage(ref Window w, Rect r)
    {
        Rect c = r.Intersect(new Rect(0, 0, w.Width, w.Height));
        if (!w.Open)
            return;
        if (w.PendingAttach >= 0) {
            // A new buffer pair after Resize goes out with its first picture: no frame of an empty buffer.
            Message a = default;
            a.Type = Protocol.Attach;
            a.Window = w.Id;
            a.Width = w.Width;
            a.Height = w.Height;
            Send(&a, w.PendingAttach);
            File.Close(w.PendingAttach);
            w.PendingAttach = -1;
            c = new Rect(0, 0, w.Width, w.Height);
        }
        if (c.IsEmpty)
            return;
        Message m = default;
        m.Type = Protocol.Damage;
        m.Window = w.Id;
        m.X = c.X;
        m.Y = c.Y;
        m.Width = c.W;
        m.Height = c.H;
        m.Value = (uint)w.Back;
        if (Send(&m) < 0)
            return;
        // Wait until the server has switched to this buffer and released the other one.
        while (true) {
            Message got = default;
            if (Receive(&got) <= 0)
                return;   // the server is gone: Next will report it
            if (got.Type == Protocol.Release && got.Window == w.Id) {
                if (got.Value == (uint)(1 - w.Back))
                    break;
                continue;
            }
            Keep(&got);
        }
        // Bring the other buffer up to date (both differ only in what this frame changed), then draw there.
        uint* from = w.Canvas.Pixels;
        int other = 1 - w.Back;
        uint* to = (uint*)(w.Base + (long)other * (long)w.BufferBytes);
        for (int y = c.Y; y < c.Bottom; y++)
            Memory.Copy(to + (long)y * w.Width + c.X, from + (long)y * w.Width + c.X, (ulong)c.W * 4);
        w.Back = other;
        PointCanvas(ref w);
    }

    public static void Present(ref Window w) => Damage(ref w, new Rect(0, 0, w.Width, w.Height));

    /// <summary>
    /// New size (after Configure, or the program's own idea): a fresh pair of buffers replaces the old one when
    /// the next Present goes out. The content is undefined (black) — redraw everything, then Present.
    /// </summary>
    public static long Resize(ref Window w, int width, int height)
    {
        if (width < 1 || height < 1 || width > 4096 || height > 4096)
            return Process.ErrBadArgument;
        byte* pixels = null;
        ulong bytes = 0;
        long shm = NewBuffers(width, height, out pixels, out bytes);
        if (shm < 0)
            return shm;
        if (w.PendingAttach >= 0)
            File.Close(w.PendingAttach);
        Device.Unmap(w.Base);   // the server keeps its own mapping until it gets the new pair
        w.PendingAttach = (int)shm;
        w.Width = width;
        w.Height = height;
        w.Base = pixels;
        w.BufferBytes = bytes;
        w.Back = 0;
        PointCanvas(ref w);
        return 0;
    }

    /// <summary>Puts <paramref name="length"/> bytes of UTF-8 text on the clipboard (the desktop keeps it).</summary>
    public static void SetClipboard(byte* text, long length)
    {
        if (_fd < 0 || length < 0)
            return;
        long shm = SharedMemory.Create(length > 0 ? (ulong)length : 1);
        if (shm < 0)
            return;
        File.WriteBytes((int)shm, text, (ulong)length);
        Message m = default;
        m.Type = Protocol.ClipboardSet;
        m.Width = (int)length;
        Send(&m, (int)shm);
        File.Close((int)shm);
    }

    /// <summary>The clipboard's text into <paramref name="buffer"/> (NUL-terminated); its length, 0 when empty.</summary>
    public static long GetClipboard(byte* buffer, long capacity)
    {
        if (_fd < 0 || capacity < 1)
            return 0;
        Message m = default;
        m.Type = Protocol.ClipboardGet;
        if (Send(&m) < 0)
            return 0;
        while (true) {
            Message got = default;
            int fdOut = -1;
            long n = Channel.Receive(_fd, &got, (ulong)sizeof(Message), &fdOut);
            if (n <= 0)
                return 0;
            if (got.Type != Protocol.ClipboardData) {
                if (fdOut >= 0)
                    File.Close(fdOut);
                if (got.Type != Protocol.Release)
                    Keep(&got);
                continue;
            }
            long length = 0;
            if (fdOut >= 0) {
                long want = got.Width < capacity - 1 ? got.Width : capacity - 1;
                File.Seek(fdOut, 0, File.SeekSet);
                length = File.ReadBytes(fdOut, buffer, (ulong)want);
                File.Close(fdOut);
                if (length < 0)
                    length = 0;
            }
            buffer[length] = 0;
            return length;
        }
    }

    public static void SetTitle(ref Window w, byte* title)
    {
        Message m = default;
        m.Type = Protocol.SetTitle;
        m.Window = w.Id;
        m.SetText(title);
        Send(&m);
    }

    public static void SetTitle(ref Window w, string title) => SetTitle(ref w, Str.Bytes(title));

    public static void Destroy(ref Window w)
    {
        if (!w.Open)
            return;
        Message m = default;
        m.Type = Protocol.Destroy;
        m.Window = w.Id;
        Send(&m);
        Device.Unmap(w.Base);
        if (w.PendingAttach >= 0)
            File.Close(w.PendingAttach);
        w.Open = false;
        w.Base = null;
        w.Canvas = new Canvas(null, 0, 0, 0);
    }

    /// <summary>
    /// The next message from the server: 1 (in <paramref name="m"/>), 0 when <paramref name="timeoutMs"/> passed
    /// (-1 = wait for ever, 0 = just look), -1 when the server is gone.
    /// </summary>
    public static int Next(out Message m, long timeoutMs)
    {
        m = default;
        if (_pendingCount > 0) {
            m = _pending[_pendingHead];
            _pendingHead = (_pendingHead + 1) % PendingMax;
            _pendingCount--;
            return 1;
        }
        if (_fd < 0)
            return -1;
        while (true) {
            PollFd p = default;
            p.Fd = _fd;
            p.Events = Poll.In;
            long ready = Poll.Wait(&p, 1, timeoutMs);
            if (ready < 0)
                return -1;
            if (ready == 0)
                return 0;
            Message got = default;
            if (Receive(&got) <= 0)
                return -1;
            if (got.Type == Protocol.Release)
                continue;   // a release nobody waits for any more
            m = got;
            return 1;
        }
    }

    /// <summary>Applies the messages that change a window by themselves (Focus); call with each message.</summary>
    public static void Track(ref Window w, Message* m)
    {
        if (m->Window != w.Id)
            return;
        if (m->Type == Protocol.Focus)
            w.Focused = m->Value != 0;
    }
}
