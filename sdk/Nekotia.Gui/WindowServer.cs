using Nekotia.User;

namespace Nekotia.Gui;

/// <summary>A connected application.</summary>
public struct ServerClient
{
    public bool Used;
    public int Fd;
    public bool Greeted;
}

/// <summary>
/// A window as the server sees it. The first block is the protocol's (filled by <see cref="WindowServer"/>);
/// the rest belongs to the desktop's policy — where the window is, whether it is minimised… — and is never
/// touched by WindowServer except that a new window starts zeroed.
/// </summary>
public unsafe struct ServerWindow
{
    public bool Used;
    public bool Dying;        // destroyed or its client left: handle WindowGone, then Release it
    public int Client;
    public uint Id;           // the client's number for it
    public int Width, Height; // of the buffer (= the content)
    public uint* Pixels;      // the buffer on show (in the client's shared memory, mapped read-only)
    public byte* Base;        // the whole mapping (both buffers when double buffered)
    public int Current;       // buffer on show
    public int OwedRelease;   // 1 + buffer whose Release could not be sent yet (the client's queue was full)
    public uint Flags;        // Protocol.Flag*
    public fixed byte Title[64];
    public bool WantsFrame;   // sent Damage since the last Frame

    // ---- policy (the desktop's)
    public int X, Y;          // screen position of the content's top-left pixel
    public bool Visible;
    public bool Minimized;
    public bool Maximized;
    public Rect Restore;      // geometry before maximising
    public int Order;         // stacking: higher is on top
    public uint Tag;          // anything
}

/// <summary>What happened, for the desktop to act on (<see cref="WindowServer.NextEvent"/>).</summary>
public struct ServerEvent
{
    public const uint WindowNew = 1, WindowGone = 2, Damage = 3, Title = 4, Resized = 5, MoveRequest = 6;

    public uint Type;
    public int Window;   // index for WindowServer.Get
    public Rect Rect;    // Damage: window coordinates; MoveRequest: X/Y; Resized: old size in W/H
}

/// <summary>
/// The protocol half of a display server, shared by every desktop: it listens for applications, maps their
/// window buffers, turns messages into <see cref="ServerEvent"/>s and sends input and window management
/// messages back. The desktop (policy: placement, decorations, focus, compositing) runs the loop:
/// <code>
/// WindowServer.Start(screenWidth, screenHeight);
/// while (true) {
///     int n = WindowServer.FillPoll(fds, 1);            // fds[0] = the desktop's own (input…)
///     Poll.Wait(fds, (uint)n, timeout);
///     WindowServer.Dispatch(fds, n);
///     while (WindowServer.NextEvent(out ServerEvent e)) …
/// }
/// </code>
/// </summary>
public static unsafe class WindowServer
{
    public const int MaxClients = 24, MaxWindows = 64, QueueSize = 512;

    static int _listener = -1;
    static ServerClient* _clients;
    static ServerWindow* _windows;
    static ServerEvent* _queue;
    static int _head, _count;
    static bool _overflow;
    static int _screenWidth, _screenHeight;
    static int _pollBase;            // where the server's fds start in the caller's poll array
    static int* _pollClient;         // poll slot → client
    static int _pollCount;

    public static int ListenFd => _listener;

    /// <summary>Starts listening under <see cref="Protocol.ServiceName"/>. False when another server runs.</summary>
    public static bool Start(int screenWidth, int screenHeight)
    {
        long fd = Channel.Listen(Protocol.ServiceName);
        if (fd < 0)
            return false;
        _listener = (int)fd;
        _clients = Memory.NewArray<ServerClient>(MaxClients);
        _windows = Memory.NewArray<ServerWindow>(MaxWindows);
        _queue = Memory.NewArray<ServerEvent>(QueueSize);
        _pollClient = Memory.NewArray<int>(MaxClients + 1);
        _screenWidth = screenWidth;
        _screenHeight = screenHeight;
        return true;
    }

    public static ServerWindow* Get(int index) => index >= 0 && index < MaxWindows ? &_windows[index] : null;

    /// <summary>True (once) when events were lost: redraw everything.</summary>
    public static bool TakeOverflow()
    {
        bool o = _overflow;
        _overflow = false;
        return o;
    }

    // ---------------------------------------------------------------- the loop

    /// <summary>Puts the listener and every client into <paramref name="fds"/> from <paramref name="at"/>; returns the new count.</summary>
    public static int FillPoll(PollFd* fds, int at)
    {
        _pollBase = at;
        int n = at;
        fds[n].Fd = _listener;
        fds[n].Events = Poll.In;
        fds[n].Ready = 0;
        _pollClient[0] = -1;
        n++;
        _pollCount = 1;
        for (int i = 0; i < MaxClients; i++) {
            if (!_clients[i].Used)
                continue;
            fds[n].Fd = _clients[i].Fd;
            fds[n].Events = Poll.In;
            fds[n].Ready = 0;
            _pollClient[_pollCount++] = i;
            n++;
        }
        return n;
    }

    /// <summary>Accepts new clients and reads what the ready ones sent (after Poll.Wait on the array FillPoll made).</summary>
    public static void Dispatch(PollFd* fds, int count)
    {
        for (int i = 0; i < MaxWindows; i++) {   // releases that did not fit last time
            if (_windows[i].Used && _windows[i].OwedRelease != 0)
                SendRelease(i);
        }
        for (int k = 0; k < _pollCount && _pollBase + k < count; k++) {
            PollFd* p = &fds[_pollBase + k];
            if (p->Ready == 0)
                continue;
            if (k == 0) {
                Accept();
                continue;
            }
            Drain(_pollClient[k]);
        }
    }

    static void Accept()
    {
        long fd = Channel.Accept(_listener);
        if (fd < 0)
            return;
        for (int i = 0; i < MaxClients; i++) {
            if (_clients[i].Used)
                continue;
            _clients[i].Used = true;
            _clients[i].Fd = (int)fd;
            _clients[i].Greeted = false;
            return;
        }
        File.Close((int)fd);   // full
    }

    /// <summary>Everything client <paramref name="c"/> has queued (up to a limit, so one client cannot starve the rest).</summary>
    static void Drain(int c)
    {
        for (int i = 0; i < 64; i++) {
            PollFd p = default;
            p.Fd = _clients[c].Fd;
            p.Events = Poll.In;
            if (Poll.Wait(&p, 1, 0) <= 0 || p.Ready == 0)
                return;
            Message m = default;
            int fdOut = -1;
            long n = Channel.Receive(_clients[c].Fd, &m, (ulong)sizeof(Message), &fdOut);
            if (n <= 0) {
                if (fdOut >= 0)
                    File.Close(fdOut);
                Disconnect(c);
                return;
            }
            Handle(c, &m, fdOut);
            if (fdOut >= 0)
                File.Close(fdOut);   // a mapping keeps what the server needs
        }
    }

    static void Disconnect(int c)
    {
        for (int i = 0; i < MaxWindows; i++) {
            if (_windows[i].Used && !_windows[i].Dying && _windows[i].Client == c)
                Kill(i);
        }
        File.Close(_clients[c].Fd);
        _clients[c].Used = false;
    }

    static int Find(int client, uint id)
    {
        for (int i = 0; i < MaxWindows; i++) {
            if (_windows[i].Used && !_windows[i].Dying && _windows[i].Client == client && _windows[i].Id == id)
                return i;
        }
        return -1;
    }

    static byte* MapBuffer(int fd, int width, int height, bool twice)
    {
        if (fd < 0 || width < 1 || height < 1 || width > 4096 || height > 4096)
            return null;
        long at = Device.Map(fd, (ulong)width * (ulong)height * 4 * (twice ? 2UL : 1UL), 0);
        return at < 0 ? null : (byte*)at;
    }

    static bool Double(ServerWindow* w) => (w->Flags & Protocol.FlagDoubleBuffered) != 0;

    static void Show(ServerWindow* w, int buffer)
    {
        w->Current = Double(w) ? buffer & 1 : 0;
        w->Pixels = (uint*)(w->Base + (long)w->Current * w->Width * w->Height * 4);
    }

    static void Handle(int c, Message* m, int fd)
    {
        if (m->Type == Protocol.Hello) {
            _clients[c].Greeted = true;
            Message w = default;
            w.Type = Protocol.Welcome;
            w.Width = _screenWidth;
            w.Height = _screenHeight;
            w.Value = Protocol.Version;
            SendTo(c, &w);
            return;
        }
        if (m->Type == Protocol.CreateWindow) {
            if (Find(c, m->Window) >= 0)
                return;
            byte* pixels = MapBuffer(fd, m->Width, m->Height, (m->Flags & Protocol.FlagDoubleBuffered) != 0);
            if (pixels == null)
                return;
            int slot = -1;
            for (int i = 0; i < MaxWindows && slot < 0; i++) {
                if (!_windows[i].Used)
                    slot = i;
            }
            if (slot < 0) {
                Device.Unmap(pixels);
                return;
            }
            ServerWindow* w = &_windows[slot];
            *w = default;
            w->Used = true;
            w->Client = c;
            w->Id = m->Window;
            w->Width = m->Width;
            w->Height = m->Height;
            w->Base = pixels;
            w->Flags = m->Flags;
            Show(w, 0);
            CopyTitle(w, m);
            Queue(ServerEvent.WindowNew, slot, new Rect(0, 0, m->Width, m->Height));
            return;
        }
        int win = Find(c, m->Window);
        if (win < 0)
            return;
        ServerWindow* sw = &_windows[win];
        switch (m->Type)
        {
            case Protocol.Attach: {
                byte* pixels = MapBuffer(fd, m->Width, m->Height, Double(sw));
                if (pixels == null)
                    return;
                Device.Unmap(sw->Base);
                Rect old = new Rect(0, 0, sw->Width, sw->Height);
                sw->Base = pixels;
                sw->Width = m->Width;
                sw->Height = m->Height;
                Show(sw, 0);
                Queue(ServerEvent.Resized, win, old);
                break;
            }
            case Protocol.Damage:
                if (Double(sw)) {
                    // Switch to the finished buffer now (we are not composing), and give the other one back.
                    Show(sw, (int)m->Value);
                    sw->OwedRelease = 2 - sw->Current;   // 1 + (1 - Current)
                    SendRelease(win);
                }
                sw->WantsFrame = true;
                Queue(ServerEvent.Damage, win, new Rect(m->X, m->Y, m->Width, m->Height).Intersect(new Rect(0, 0, sw->Width, sw->Height)));
                break;
            case Protocol.SetTitle:
                CopyTitle(sw, m);
                Queue(ServerEvent.Title, win, new Rect(0, 0, 0, 0));
                break;
            case Protocol.Destroy:
                Kill(win);
                break;
            case Protocol.Move:
                Queue(ServerEvent.MoveRequest, win, new Rect(m->X, m->Y, 0, 0));
                break;
        }
    }

    static void CopyTitle(ServerWindow* w, Message* m)
    {
        for (int i = 0; i < 63; i++) {
            w->Title[i] = m->Text[i];
        }
        w->Title[63] = 0;
    }

    /// <summary>The window is going: the desktop gets WindowGone and calls Release when it has let go of it.</summary>
    static void Kill(int i)
    {
        _windows[i].Dying = true;
        Queue(ServerEvent.WindowGone, i, new Rect(0, 0, _windows[i].Width, _windows[i].Height));
    }

    /// <summary>Frees a window after its WindowGone (unmaps the buffer).</summary>
    public static void Release(int index)
    {
        ServerWindow* w = Get(index);
        if (w == null || !w->Used)
            return;
        if (w->Base != null)
            Device.Unmap(w->Base);
        *w = default;
    }

    static void Queue(uint type, int window, Rect r)
    {
        if (_count == QueueSize) {
            _overflow = true;
            if (type == ServerEvent.Damage)
                return;   // a full redraw covers it
            // structural events must not get lost: drop the oldest damage instead
            for (int k = 0; k < _count; k++) {
                int at = (_head + k) % QueueSize;
                if (_queue[at].Type == ServerEvent.Damage) {
                    _queue[at] = _queue[(_head + _count - 1) % QueueSize];
                    _count--;
                    break;
                }
            }
            if (_count == QueueSize)
                return;
        }
        ServerEvent* e = &_queue[(_head + _count) % QueueSize];
        e->Type = type;
        e->Window = window;
        e->Rect = r;
        _count++;
    }

    public static bool NextEvent(out ServerEvent e)
    {
        e = default;
        if (_count == 0)
            return false;
        e = _queue[_head];
        _head = (_head + 1) % QueueSize;
        _count--;
        return true;
    }

    // ---------------------------------------------------------------- server → client

    static void SendTo(int client, Message* m)
    {
        if (client < 0 || client >= MaxClients || !_clients[client].Used)
            return;
        // Never wait for a client: when its queue is full the message is dropped (a stuck client only hurts itself).
        Channel.Send(_clients[client].Fd, m, (ulong)sizeof(Message), -1);
    }

    /// The client waits for this one, so it must not be lost: kept owed until it fits into the queue.
    static void SendRelease(int index)
    {
        ServerWindow* w = &_windows[index];
        if (w->Dying || !_clients[w->Client].Used) {
            w->OwedRelease = 0;
            return;
        }
        Message m = default;
        m.Type = Protocol.Release;
        m.Window = w->Id;
        m.Value = (uint)(w->OwedRelease - 1);
        if (Channel.Send(_clients[w->Client].Fd, &m, (ulong)sizeof(Message), -1) != Process.ErrBusy)
            w->OwedRelease = 0;
    }

    static void SendWindow(int index, Message* m)
    {
        ServerWindow* w = Get(index);
        if (w == null || !w->Used || w->Dying)
            return;
        m->Window = w->Id;
        SendTo(w->Client, m);
    }

    public static void SendKey(int index, InputEvent* e)
    {
        Message m = default;
        m.Type = Protocol.Key;
        m.Code = e->Code;
        m.Value = e->Pressed;
        m.Codepoint = e->Codepoint;
        m.Mods = e->Mods;
        SendWindow(index, &m);
    }

    /// <summary>Pointer event at (x, y) in window coordinates; kind = Protocol.Pointer*.</summary>
    public static void SendPointer(int index, uint kind, int x, int y, uint code, uint value, uint mods)
    {
        Message m = default;
        m.Type = Protocol.Pointer;
        m.Code = kind;
        m.X = x;
        m.Y = y;
        m.Value = value;
        m.Codepoint = code;   // the button, for PointerButton
        m.Mods = mods;
        SendWindow(index, &m);
    }

    public static void SendFocus(int index, bool focused)
    {
        Message m = default;
        m.Type = Protocol.Focus;
        m.Value = focused ? 1u : 0u;
        SendWindow(index, &m);
    }

    public static void SendClose(int index)
    {
        Message m = default;
        m.Type = Protocol.Close;
        SendWindow(index, &m);
    }

    public static void SendConfigure(int index, int width, int height)
    {
        Message m = default;
        m.Type = Protocol.Configure;
        m.Width = width;
        m.Height = height;
        SendWindow(index, &m);
    }

    /// <summary>After composing: tells every window that asked that its damage is on screen.</summary>
    public static void SendFrames()
    {
        for (int i = 0; i < MaxWindows; i++) {
            if (!_windows[i].Used || _windows[i].Dying || !_windows[i].WantsFrame)
                continue;
            _windows[i].WantsFrame = false;
            Message m = default;
            m.Type = Protocol.Frame;
            SendWindow(i, &m);
        }
    }
}
