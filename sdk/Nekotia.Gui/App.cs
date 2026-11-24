using Nekotia.User;

namespace Nekotia.Gui;

/// <summary>A window of this program: its pixels (shared with the display server) and its size.</summary>
public unsafe struct Window
{
    public uint Id;
    public int Width, Height;
    public Canvas Canvas;     // draw here, then App.Present / App.Damage
    public bool Open;
    public bool Focused;
    public ulong BufferBytes;
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
    static int _fd = -1;
    static int _screenWidth, _screenHeight;
    static uint _nextId = 1;
    static Font _font;

    /// <summary>The channel to the server (to poll it together with other fds).</summary>
    public static int Fd => _fd;
    public static int ScreenWidth => _screenWidth;
    public static int ScreenHeight => _screenHeight;

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
        Message m = default;
        m.Type = Protocol.Hello;
        m.Value = Protocol.Version;
        Send(&m);
        // Wait for Welcome (nothing else comes before it).
        while (true) {
            int fdOut = -1;
            long n = Channel.Receive(_fd, &m, (ulong)sizeof(Message), &fdOut);
            if (fdOut >= 0)
                File.Close(fdOut);
            if (n <= 0) {
                File.Close(_fd);
                _fd = -1;
                return n < 0 ? n : Process.ErrBrokenPipe;
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

    /// <summary>A shared buffer of width × height pixels, mapped here: its fd (to attach) and address.</summary>
    static long NewBuffer(int width, int height, out uint* pixels, out ulong bytes)
    {
        pixels = null;
        bytes = (ulong)width * (ulong)height * 4;
        long shm = SharedMemory.Create(bytes);
        if (shm < 0)
            return shm;
        long at = Device.Map((int)shm, bytes, Device.MapWrite);
        if (at < 0) {
            File.Close((int)shm);
            return at;
        }
        pixels = (uint*)at;
        return shm;
    }

    /// <summary>
    /// Opens a window (title UTF-8, size of its content without decorations). Flags: Protocol.Flag*.
    /// The content starts black: draw, then Present.
    /// </summary>
    public static long CreateWindow(byte* title, int width, int height, uint flags, out Window window)
    {
        window = default;
        if (_fd < 0)
            return Process.ErrBadFd;
        if (width < 1 || height < 1 || width > 4096 || height > 4096)
            return Process.ErrBadArgument;
        uint* pixels = null;
        ulong bytes = 0;
        long shm = NewBuffer(width, height, out pixels, out bytes);
        if (shm < 0)
            return shm;
        window.Id = _nextId++;
        window.Width = width;
        window.Height = height;
        window.Canvas = new Canvas(pixels, width, height, width);
        window.Open = true;
        window.BufferBytes = bytes;

        Message m = default;
        m.Type = Protocol.CreateWindow;
        m.Window = window.Id;
        m.Width = width;
        m.Height = height;
        m.Flags = flags;
        m.SetText(title);
        long r = Send(&m, (int)shm);
        File.Close((int)shm);   // the mapping (and the server's copy) keep the memory
        return r < 0 ? r : 0;
    }

    public static long CreateWindow(string title, int width, int height, uint flags, out Window window) =>
        CreateWindow(Str.Bytes(title), width, height, flags, out window);

    /// <summary>Part of the window changed: the server shows it with the next frame.</summary>
    public static void Damage(ref Window w, Rect r)
    {
        Rect c = r.Intersect(new Rect(0, 0, w.Width, w.Height));
        if (c.IsEmpty || !w.Open)
            return;
        Message m = default;
        m.Type = Protocol.Damage;
        m.Window = w.Id;
        m.X = c.X;
        m.Y = c.Y;
        m.Width = c.W;
        m.Height = c.H;
        Send(&m);
    }

    public static void Present(ref Window w) => Damage(ref w, new Rect(0, 0, w.Width, w.Height));

    /// <summary>
    /// New size (after Configure, or the program's own idea): a fresh buffer replaces the old one. The content
    /// is undefined (black) — redraw and Present.
    /// </summary>
    public static long Resize(ref Window w, int width, int height)
    {
        if (width < 1 || height < 1 || width > 4096 || height > 4096)
            return Process.ErrBadArgument;
        uint* pixels = null;
        ulong bytes = 0;
        long shm = NewBuffer(width, height, out pixels, out bytes);
        if (shm < 0)
            return shm;
        Message m = default;
        m.Type = Protocol.Attach;
        m.Window = w.Id;
        m.Width = width;
        m.Height = height;
        long r = Send(&m, (int)shm);
        File.Close((int)shm);
        Device.Unmap(w.Canvas.Pixels);
        w.Width = width;
        w.Height = height;
        w.Canvas = new Canvas(pixels, width, height, width);
        w.BufferBytes = bytes;
        return r < 0 ? r : 0;
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
        Device.Unmap(w.Canvas.Pixels);
        w.Open = false;
        w.Canvas = new Canvas(null, 0, 0, 0);
    }

    /// <summary>
    /// The next message from the server: 1 (in <paramref name="m"/>), 0 when <paramref name="timeoutMs"/> passed
    /// (-1 = wait for ever, 0 = just look), -1 when the server is gone.
    /// </summary>
    public static int Next(out Message m, long timeoutMs)
    {
        m = default;
        if (_fd < 0)
            return -1;
        PollFd p = default;
        p.Fd = _fd;
        p.Events = Poll.In;
        long ready = Poll.Wait(&p, 1, timeoutMs);
        if (ready < 0)
            return -1;
        if (ready == 0)
            return 0;
        int fdOut = -1;
        Message got = default;
        long n = Channel.Receive(_fd, &got, (ulong)sizeof(Message), &fdOut);
        if (fdOut >= 0)
            File.Close(fdOut);
        if (n <= 0)
            return -1;
        m = got;
        return 1;
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
