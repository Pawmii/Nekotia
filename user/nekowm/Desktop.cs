// nekowm — the first Nekotia desktop: a stacking window manager with a CPU compositor.
//
//   /dev/fb0 + /dev/input ──► Desktop (this file: input, focus, moving/resizing, the loop)
//                             Render (compositing damaged rectangles: wallpaper, shadows, decorations,
//                                     window contents, panel, menu, cursor)
//   applications ◄─ window protocol ─► WindowServer (sdk/Nekotia.Gui)
//
// Keys: Alt+Tab next window, Alt+F4 close, Ctrl+Alt+T terminal, Super (Win) launcher, Ctrl+Alt+Backspace (kernel)
// kills the desktop. Started from either shell with `nekowm`; leaving it ("Exit desktop") gives the screen back
// to the console.
using Nekotia;
using Nekotia.Gui;
using Nekotia.User;

namespace NekoWm;

/// One entry of the launcher menu (/etc/nekowm/apps).
public unsafe struct AppEntry
{
    public fixed byte Name[40];
    public fixed byte Path[64];
    public fixed byte Args[64];
}

public static unsafe class Desktop
{
    public const int TitleHeight = 30;
    public const int PanelHeight = 38;
    public const int ShadowSize = 16;
    public const int ShadowOffset = 4;
    public const int MaxApps = 16;

    // Areas of the screen the pointer can be over (HitTest).
    public const int AreaNone = 0, AreaContent = 1, AreaTitle = 2, AreaClose = 3, AreaMaximize = 4, AreaMinimize = 5,
        AreaResize = 6, AreaPanel = 7, AreaLauncherButton = 8, AreaTask = 9, AreaMenu = 10;

    // Drag modes
    const int DragNone = 0, DragMove = 1, DragResize = 2;

    public static Screen* Output;
    public static Canvas* Back;      // the compositor's back buffer
    public static Font* UiFont;
    public static Canvas* Wallpaper;
    public static int Width, Height;
    public static Damage Dirty;

    // stacking order, bottom first
    public static int* Stack;
    public static int StackCount;
    public static int Focus = -1;

    // pointer
    public static int PointerX, PointerY;
    public static int CursorShape;
    static uint _buttons;
    static byte _mods;
    static int _grab = -1;            // window that gets the pointer while a button is held in its content
    static int _pointerWindow = -1;   // window whose content the pointer is over (Enter/Leave)
    public static int HoverArea, HoverIndex = -1;

    // dragging
    static int _drag, _dragWindow = -1, _dragOffsetX, _dragOffsetY;
    public static bool Resizing;
    public static Rect ResizeOutline;
    static ulong _lastTitleClick;
    static int _lastTitleWindow = -1;

    // launcher
    public static AppEntry* Apps;
    public static int AppCount;
    public static bool MenuOpen;
    static bool _superAlone;
    static bool _quit;

    static InputDevice _input;
    static bool _stats;    // nekowm -v: frame timings on stdout every 2 s
    static ulong _mhz;

    public static int Main()
    {
        _stats = Process.ArgCount() > 1 && Str.Equals(Process.Arg(1), "-v");
        _mhz = CpuMhz();
        Output = Memory.New<Screen>();
        long r = Screen.Open(0, out *Output);
        if (r < 0) {
            Console.WriteError("nekowm: cannot open /dev/fb0: ");
            Console.WriteLine(Process.ErrorText(r));
            return 1;
        }
        Width = Output->Width;
        Height = Output->Height;
        Back = &Output->Back;
        UiFont = Memory.New<Font>();
        if (!Font.Load(Font.DefaultPath, out *UiFont)) {
            Output->Close();
            Console.WriteLine("nekowm: no font at /usr/share/fonts/ter-v16n.psf");
            return 1;
        }
        r = InputDevice.Open(out _input);
        if (r < 0) {
            Output->Close();
            Console.WriteError("nekowm: cannot open /dev/input: ");
            Console.WriteLine(Process.ErrorText(r));
            return 1;
        }
        if (!WindowServer.Start(Width, Height)) {
            _input.Close();
            Output->Close();
            Console.WriteLine("nekowm: another display server is running");
            return 1;
        }
        Stack = Memory.NewArray<int>(WindowServer.MaxWindows);
        LoadApps();
        Wallpaper = Memory.New<Canvas>();
        *Wallpaper = Canvas.Create(Width, Height);
        Render.PaintWallpaper(Wallpaper);
        PointerX = Width / 2;
        PointerY = Height / 2;
        DamageAll();
        Run();

        _input.Close();
        Output->Close();
        return 0;
    }

    // ---------------------------------------------------------------- main loop

    static void Run()
    {
        PollFd* fds = stackalloc PollFd[WindowServer.MaxClients + 4];
        InputEvent* events = stackalloc InputEvent[64];
        long lastMinute = -1;
        ulong lastReport = Time.UptimeMs();
        ulong nextFrame = 0;   // uptime of the vertical blank the next frame waits for
        while (!_quit) {
            fds[0].Fd = _input.Fd;
            fds[0].Events = Poll.In;
            fds[0].Ready = 0;
            int n = WindowServer.FillPoll(fds, 1);
            // Pacing (vsync): with something to draw, wait at most until the next vertical blank and keep
            // gathering input meanwhile — one frame per refresh, however fast the mouse moves.
            ulong now = Time.UptimeMs();
            long timeout = Dirty.IsEmpty ? 1000 : (now >= nextFrame ? 0 : (long)(nextFrame - now));
            Poll.Wait(fds, (uint)n, timeout);

            if (fds[0].Ready != 0) {
                int got = _input.Read(events, 64);
                for (int i = 0; i < got; i++)
                    OnInput(&events[i]);
            }
            WindowServer.Dispatch(fds, n);
            ServerEvent e = default;
            while (WindowServer.NextEvent(out e))
                OnServerEvent(&e);
            if (WindowServer.TakeOverflow())
                DamageAll();

            long minute = Clock.Now() / 60;
            if (minute != lastMinute) {
                lastMinute = minute;
                DamageRect(Render.ClockRect());
            }
            if (!Dirty.IsEmpty && Time.UptimeMs() >= nextFrame) {
                Render.Compose();
                WindowServer.SendFrames();
                nextFrame = Output->NextVblankMs();
            }
            if (_stats && Time.UptimeMs() - lastReport >= 2000) {
                Render.Report(_mhz, Time.UptimeMs() - lastReport);
                lastReport = Time.UptimeMs();
            }
        }
    }

    /// "cpu MHz:" from /proc/cpuinfo (for the profiling numbers).
    static ulong CpuMhz()
    {
        byte* text = stackalloc byte[512];
        long fd = File.Open("/proc/cpuinfo", File.Read);
        if (fd < 0)
            return 1000;
        long n = File.ReadBytes((int)fd, text, 511);
        File.Close((int)fd);
        text[n < 0 ? 0 : n] = 0;
        for (byte* p = text; *p != 0; p++) {
            if (Str.StartsWith(p, "cpu MHz:")) {
                p += 8;
                while (*p == ' ')
                    p++;
                ulong v = 0;
                while (*p >= '0' && *p <= '9')
                    v = v * 10 + (ulong)(*p++ - '0');
                return v > 0 ? v : 1000;
            }
        }
        return 1000;
    }

    // ---------------------------------------------------------------- damage helpers

    public static void DamageRect(Rect r) => Dirty.Add(r.Intersect(new Rect(0, 0, Width, Height)));

    public static void DamageAll() => DamageRect(new Rect(0, 0, Width, Height));

    public static bool Decorated(ServerWindow* w) => (w->Flags & Protocol.FlagNoDecorations) == 0;

    /// The window with its title bar and border.
    public static Rect Frame(ServerWindow* w)
    {
        if (!Decorated(w))
            return new Rect(w->X, w->Y, w->Width, w->Height);
        return new Rect(w->X - 1, w->Y - TitleHeight - 1, w->Width + 2, w->Height + TitleHeight + 2);
    }

    /// Everything a window paints, shadow included.
    public static Rect Extent(ServerWindow* w) => Frame(w).Inflate(ShadowSize).Offset(0, ShadowOffset).Union(Frame(w));

    public static void DamageWindow(int i)
    {
        ServerWindow* w = WindowServer.Get(i);
        if (w != null && w->Used)
            DamageRect(Extent(w));
    }

    public static bool Shown(ServerWindow* w) => w != null && w->Used && !w->Dying && w->Visible && !w->Minimized;

    // Title bar buttons: centres, from the right edge of the frame.
    public static Point ButtonCentre(ServerWindow* w, int which)   // 0 close, 1 maximise, 2 minimise
    {
        Rect f = Frame(w);
        int slot = which == 2 && (w->Flags & Protocol.FlagFixedSize) != 0 ? 1 : which;   // no maximise: close the gap
        return new Point(f.Right - 20 - slot * 24, f.Y + 1 + TitleHeight / 2);
    }

    // ---------------------------------------------------------------- stacking and focus

    static void StackRemove(int i)
    {
        for (int k = 0; k < StackCount; k++) {
            if (Stack[k] != i)
                continue;
            for (int j = k + 1; j < StackCount; j++)
                Stack[j - 1] = Stack[j];
            StackCount--;
            return;
        }
    }

    static void Raise(int i)
    {
        StackRemove(i);
        Stack[StackCount++] = i;
        DamageWindow(i);
    }

    static void SetFocus(int i)
    {
        if (i == Focus)
            return;
        if (Focus >= 0) {
            WindowServer.SendFocus(Focus, false);
            DamageWindow(Focus);
        }
        Focus = i;
        if (Focus >= 0) {
            WindowServer.SendFocus(Focus, true);
            DamageWindow(Focus);
        }
        DamageRect(Render.PanelRect());
    }

    public static void Activate(int i)
    {
        ServerWindow* w = WindowServer.Get(i);
        if (w == null || !w->Used)
            return;
        if (w->Minimized) {
            w->Minimized = false;
            DamageWindow(i);
        }
        Raise(i);
        SetFocus(i);
    }

    /// The topmost shown window other than <paramref name="except"/>.
    static int TopWindow(int except)
    {
        for (int k = StackCount - 1; k >= 0; k--) {
            int i = Stack[k];
            if (i != except && Shown(WindowServer.Get(i)))
                return i;
        }
        return -1;
    }

    static void Minimize(int i)
    {
        ServerWindow* w = WindowServer.Get(i);
        DamageWindow(i);
        w->Minimized = true;
        if (Focus == i)
            SetFocus(TopWindow(i));
        DamageRect(Render.PanelRect());
    }

    static void ToggleMaximize(int i)
    {
        ServerWindow* w = WindowServer.Get(i);
        if ((w->Flags & Protocol.FlagFixedSize) != 0)
            return;
        if (w->Maximized) {
            w->Maximized = false;
            DamageWindow(i);
            w->X = w->Restore.X;
            w->Y = w->Restore.Y;
            WindowServer.SendConfigure(i, w->Restore.W, w->Restore.H);
            return;
        }
        w->Restore = new Rect(w->X, w->Y, w->Width, w->Height);
        w->Maximized = true;
        DamageWindow(i);
        w->X = 1;
        w->Y = TitleHeight + 1;
        WindowServer.SendConfigure(i, Width - 2, Height - PanelHeight - TitleHeight - 2);
    }

    /// Where a new window goes: centred, a little lower and to the right for each one already open.
    static void Place(ServerWindow* w)
    {
        int open = 0;
        for (int k = 0; k < StackCount; k++) {
            if (Shown(WindowServer.Get(Stack[k])))
                open++;
        }
        int x = (Width - w->Width) / 2 + (open % 6) * 28 - 70;
        int y = (Height - PanelHeight - w->Height - TitleHeight) / 2 + TitleHeight + (open % 6) * 28 - 70;
        if ((w->Flags & Protocol.FlagCentered) != 0) {
            x = (Width - w->Width) / 2;
            y = (Height - PanelHeight - w->Height + TitleHeight) / 2;
        }
        w->X = Clamp(x, 1, Width - w->Width - 1);
        w->Y = Clamp(y, TitleHeight + 1, Height - PanelHeight - w->Height - 1);
    }

    public static int Clamp(int v, int min, int max)
    {
        if (max < min)
            return min;
        return v < min ? min : v > max ? max : v;
    }

    // ---------------------------------------------------------------- protocol events

    static void OnServerEvent(ServerEvent* e)
    {
        ServerWindow* w = WindowServer.Get(e->Window);
        switch (e->Type)
        {
            case ServerEvent.WindowNew:
                Place(w);
                w->Visible = true;
                Raise(e->Window);
                SetFocus(e->Window);
                break;
            case ServerEvent.WindowGone: {
                DamageWindow(e->Window);
                StackRemove(e->Window);
                if (_grab == e->Window)
                    _grab = -1;
                if (_pointerWindow == e->Window)
                    _pointerWindow = -1;
                if (_dragWindow == e->Window) {
                    _drag = DragNone;
                    _dragWindow = -1;
                    if (Resizing) {
                        Resizing = false;
                        DamageRect(ResizeOutline.Inflate(2));
                    }
                }
                bool focused = Focus == e->Window;
                WindowServer.Release(e->Window);
                if (focused) {
                    Focus = -1;
                    SetFocus(TopWindow(-1));
                }
                DamageRect(Render.PanelRect());
                break;
            }
            case ServerEvent.Damage:
                if (Shown(w))
                    DamageRect(e->Rect.Offset(w->X, w->Y));
                break;
            case ServerEvent.Title:
                DamageRect(Frame(w));
                DamageRect(Render.PanelRect());
                break;
            case ServerEvent.Resized: {
                Rect old = new Rect(w->X, w->Y, e->Rect.W, e->Rect.H);
                ServerWindow before = *w;
                before.Width = e->Rect.W;
                before.Height = e->Rect.H;
                DamageRect(Extent(&before));
                DamageWindow(e->Window);
                break;
            }
            case ServerEvent.MoveRequest:
                DamageWindow(e->Window);
                w->X = Clamp(e->Rect.X, 1, Width - w->Width - 1);
                w->Y = Clamp(e->Rect.Y, TitleHeight + 1, Height - PanelHeight - 1);
                DamageWindow(e->Window);
                break;
        }
    }

    // ---------------------------------------------------------------- input

    static void OnInput(InputEvent* e)
    {
        switch (e->Type)
        {
            case InputEvent.Key:
                OnKey(e);
                break;
            case InputEvent.Motion:
                MovePointer(PointerX + e->X, PointerY + e->Y);
                break;
            case InputEvent.Absolute:
                MovePointer((int)((long)e->X * (Width - 1) / 32767), (int)((long)e->Y * (Height - 1) / 32767));
                break;
            case InputEvent.Button:
                OnButton((int)e->Code, e->Pressed != 0);
                break;
            case InputEvent.Wheel: {
                int over = WindowAt(PointerX, PointerY);
                int idx = -1;
                if (over >= 0 && HitTest(PointerX, PointerY, out idx) == AreaContent) {
                    ServerWindow* w = WindowServer.Get(over);
                    WindowServer.SendPointer(over, Protocol.PointerWheel, PointerX - w->X, PointerY - w->Y, 0, (uint)e->Y, _mods);
                }
                break;
            }
        }
    }

    static void OnKey(InputEvent* e)
    {
        _mods = e->Mods;
        bool alt = (e->Mods & Keys.ModAlt) != 0, ctrl = (e->Mods & Keys.ModCtrl) != 0;
        if (e->Code == Keys.LeftSuper || e->Code == Keys.RightSuper) {
            if (e->Pressed != 0) {
                _superAlone = true;
            } else if (_superAlone) {
                ToggleMenu();
            }
            return;
        }
        if (e->Pressed != 0)
            _superAlone = false;
        if (e->Pressed != 0) {
            if (alt && e->Code == Keys.Tab) {
                CycleFocus();
                return;
            }
            if (alt && e->Code == Keys.F4) {
                if (Focus >= 0)
                    WindowServer.SendClose(Focus);
                return;
            }
            if (ctrl && alt && e->Code == 0x14) {   // T
                Launch(Str.Bytes("/bin/term"), null);
                return;
            }
            if (MenuOpen && e->Code == Keys.Escape) {
                ToggleMenu();
                return;
            }
        }
        if (Focus >= 0)
            WindowServer.SendKey(Focus, e);
    }

    static void CycleFocus()
    {
        // the lowest shown window comes to the top
        for (int k = 0; k < StackCount; k++) {
            int i = Stack[k];
            if (Shown(WindowServer.Get(i)) && i != Focus) {
                Activate(i);
                return;
            }
        }
        // only minimised ones: bring one back
        for (int k = 0; k < StackCount; k++) {
            ServerWindow* w = WindowServer.Get(Stack[k]);
            if (w->Used && !w->Dying && w->Minimized) {
                Activate(Stack[k]);
                return;
            }
        }
    }

    static void MovePointer(int x, int y)
    {
        x = Clamp(x, 0, Width - 1);
        y = Clamp(y, 0, Height - 1);
        if (x == PointerX && y == PointerY)
            return;
        DamageRect(Cursor.Bounds(CursorShape, PointerX, PointerY));
        PointerX = x;
        PointerY = y;

        if (_drag == DragMove) {
            ServerWindow* w = WindowServer.Get(_dragWindow);
            DamageWindow(_dragWindow);
            w->X = Clamp(x - _dragOffsetX, 40 - w->Width, Width - 40);
            w->Y = Clamp(y - _dragOffsetY, TitleHeight + 1, Height - PanelHeight - 4);
            DamageWindow(_dragWindow);
        } else if (_drag == DragResize) {
            ServerWindow* w = WindowServer.Get(_dragWindow);
            DamageRect(ResizeOutline.Inflate(2));
            int newW = Clamp(x - w->X + _dragOffsetX, 120, Width);
            int newH = Clamp(y - w->Y + _dragOffsetY, 60, Height);
            ResizeOutline = new Rect(w->X - 1, w->Y - TitleHeight - 1, newW + 2, newH + TitleHeight + 2);
            DamageRect(ResizeOutline.Inflate(2));
        } else if (_grab >= 0) {
            ServerWindow* w = WindowServer.Get(_grab);
            WindowServer.SendPointer(_grab, Protocol.PointerMove, x - w->X, y - w->Y, 0, _buttons, _mods);
        } else {
            UpdateHover();
        }
        DamageRect(Cursor.Bounds(CursorShape, PointerX, PointerY));
    }

    /// Hover highlight, Enter/Leave for the window under the pointer, cursor shape.
    static void UpdateHover()
    {
        int index = -1;
        int area = HitTest(PointerX, PointerY, out index);
        if (area != HoverArea || index != HoverIndex) {
            DamageHover();
            HoverArea = area;
            HoverIndex = index;
            DamageHover();
        }
        int shape = area == AreaResize ? Cursor.Resize : Cursor.Arrow;
        if (shape != CursorShape) {
            DamageRect(Cursor.Bounds(CursorShape, PointerX, PointerY));
            CursorShape = shape;
        }
        int under = area == AreaContent ? index : -1;
        if (under != _pointerWindow) {
            if (_pointerWindow >= 0)
                WindowServer.SendPointer(_pointerWindow, Protocol.PointerLeave, 0, 0, 0, 0, _mods);
            _pointerWindow = under;
            if (under >= 0) {
                ServerWindow* w = WindowServer.Get(under);
                WindowServer.SendPointer(under, Protocol.PointerEnter, PointerX - w->X, PointerY - w->Y, 0, 0, _mods);
            }
        }
        if (under >= 0) {
            ServerWindow* w = WindowServer.Get(under);
            WindowServer.SendPointer(under, Protocol.PointerMove, PointerX - w->X, PointerY - w->Y, 0, _buttons, _mods);
        }
    }

    static void DamageHover()
    {
        if (HoverArea == AreaClose || HoverArea == AreaMaximize || HoverArea == AreaMinimize || HoverArea == AreaTitle) {
            ServerWindow* w = WindowServer.Get(HoverIndex);
            if (w != null && w->Used)
                DamageRect(Frame(w));
        } else if (HoverArea == AreaLauncherButton || HoverArea == AreaTask || HoverArea == AreaPanel) {
            DamageRect(Render.PanelRect());
        } else if (HoverArea == AreaMenu) {
            DamageRect(Render.MenuRect());
        }
    }

    static void OnButton(int button, bool pressed)
    {
        uint bit = 1u << button;
        _buttons = pressed ? _buttons | bit : _buttons & ~bit;

        if (!pressed) {
            if (_drag != DragNone && button == 0) {
                if (_drag == DragResize) {
                    ServerWindow* w = WindowServer.Get(_dragWindow);
                    DamageRect(ResizeOutline.Inflate(2));
                    Resizing = false;
                    w->Maximized = false;
                    WindowServer.SendConfigure(_dragWindow, ResizeOutline.W - 2, ResizeOutline.H - TitleHeight - 2);
                }
                _drag = DragNone;
                _dragWindow = -1;
                DamageRect(Cursor.Bounds(CursorShape, PointerX, PointerY));
                CursorShape = Cursor.Arrow;
                UpdateHover();
                return;
            }
            if (_grab >= 0) {
                ServerWindow* w = WindowServer.Get(_grab);
                WindowServer.SendPointer(_grab, Protocol.PointerButton, PointerX - w->X, PointerY - w->Y, (uint)button, 0, _mods);
                if (_buttons == 0) {
                    _grab = -1;
                    UpdateHover();
                }
            }
            return;
        }

        int index = -1;
        int area = HitTest(PointerX, PointerY, out index);
        if (MenuOpen && area != AreaMenu && area != AreaLauncherButton)
            ToggleMenu();
        switch (area)
        {
            case AreaLauncherButton:
                ToggleMenu();
                return;
            case AreaMenu:
                MenuClick(index);
                return;
            case AreaTask: {
                ServerWindow* w = WindowServer.Get(index);
                if (index == Focus && !w->Minimized)
                    Minimize(index);
                else
                    Activate(index);
                return;
            }
            case AreaPanel:
            case AreaNone:
                return;
        }

        // A window.
        ServerWindow* win = WindowServer.Get(index);
        Activate(index);
        switch (area)
        {
            case AreaContent:
                _grab = index;
                WindowServer.SendPointer(index, Protocol.PointerButton, PointerX - win->X, PointerY - win->Y, (uint)button, 1, _mods);
                break;
            case AreaTitle: {
                if (button != 0)
                    break;
                ulong now = Time.UptimeMs();
                if (_lastTitleWindow == index && now - _lastTitleClick < 400) {
                    _lastTitleWindow = -1;
                    ToggleMaximize(index);
                    break;
                }
                _lastTitleClick = now;
                _lastTitleWindow = index;
                if (win->Maximized)
                    break;
                _drag = DragMove;
                _dragWindow = index;
                _dragOffsetX = PointerX - win->X;
                _dragOffsetY = PointerY - win->Y;
                DamageRect(Cursor.Bounds(CursorShape, PointerX, PointerY));
                CursorShape = Cursor.Move;
                break;
            }
            case AreaClose:
                WindowServer.SendClose(index);
                break;
            case AreaMaximize:
                ToggleMaximize(index);
                break;
            case AreaMinimize:
                Minimize(index);
                break;
            case AreaResize:
                if (button != 0)
                    break;
                _drag = DragResize;
                _dragWindow = index;
                // keep the grab point where it is relative to the corner
                _dragOffsetX = win->Width - (PointerX - win->X);
                _dragOffsetY = win->Height - (PointerY - win->Y);
                Resizing = true;
                ResizeOutline = Frame(win);
                DamageRect(ResizeOutline.Inflate(2));
                break;
        }
    }

    /// What is at (x, y); `index` = the window, task or menu item.
    public static int HitTest(int x, int y, out int index)
    {
        index = -1;
        if (MenuOpen && Render.MenuRect().Contains(x, y)) {
            index = Render.MenuItemAt(x, y);
            return AreaMenu;
        }
        if (Render.PanelRect().Contains(x, y)) {
            if (Render.LauncherButtonRect().Contains(x, y))
                return AreaLauncherButton;
            index = Render.TaskAt(x, y);
            return index >= 0 ? AreaTask : AreaPanel;
        }
        int i = WindowAt(x, y);
        if (i < 0)
            return AreaNone;
        index = i;
        ServerWindow* w = WindowServer.Get(i);
        Rect f = Frame(w);
        bool resizable = (w->Flags & Protocol.FlagFixedSize) == 0 && !w->Maximized;
        if (resizable && x >= f.Right - 14 && y >= f.Bottom - 14)
            return AreaResize;
        if (Decorated(w) && y < w->Y) {
            for (int b = 0; b < 3; b++) {
                Point c = ButtonCentre(w, b);
                int dx = x - c.X, dy = y - c.Y;
                if (dx * dx + dy * dy <= 81) {
                    if (b == 1 && (w->Flags & Protocol.FlagFixedSize) != 0)
                        continue;
                    return b == 0 ? AreaClose : b == 1 ? AreaMaximize : AreaMinimize;
                }
            }
            return AreaTitle;
        }
        if (resizable && (x >= f.Right - 4 || y >= f.Bottom - 4))
            return AreaResize;
        return AreaContent;
    }

    /// The topmost shown window whose frame contains (x, y).
    public static int WindowAt(int x, int y)
    {
        for (int k = StackCount - 1; k >= 0; k--) {
            int i = Stack[k];
            ServerWindow* w = WindowServer.Get(i);
            if (Shown(w) && Frame(w).Contains(x, y))
                return i;
        }
        return -1;
    }

    // ---------------------------------------------------------------- launcher

    static void ToggleMenu()
    {
        MenuOpen = !MenuOpen;
        DamageRect(Render.MenuRect().Inflate(ShadowSize));
        DamageRect(Render.PanelRect());
    }

    static void MenuClick(int item)
    {
        if (item < 0)
            return;
        ToggleMenu();
        if (item < AppCount) {
            Launch(Apps[item].Path, Apps[item].Args[0] != 0 ? Apps[item].Args : null);
            return;
        }
        if (item == AppCount)
            _quit = true;   // Exit desktop
    }

    /// Starts a program; a helper thread waits for it so it does not linger as a zombie.
    public static void Launch(byte* path, byte* args)
    {
        long pid = Process.Start(path, args);
        if (pid < 0)
            return;
        Thread.Start(&Reap, (ulong)pid);
    }

    static void Reap(ulong pid) => Process.Wait((uint)pid);

    static void LoadApps()
    {
        Apps = Memory.NewArray<AppEntry>(MaxApps);
        long fd = File.Open("/etc/nekowm/apps", File.Read);
        if (fd < 0)
            return;
        byte* text = (byte*)Memory.AllocZeroed(4096);
        long n = File.ReadBytes((int)fd, text, 4095);
        File.Close((int)fd);
        if (n < 0)
            n = 0;
        byte* line = text;
        while (*line != 0 && AppCount < MaxApps) {
            byte* end = line;
            while (*end != 0 && *end != '\n')
                end++;
            byte next = *end;
            *end = 0;
            if (*line != '#' && *line != 0)
                ParseApp(line, &Apps[AppCount]);
            *end = next;
            line = next == 0 ? end : end + 1;
        }
        Memory.Free(text);
    }

    /// "Name;/bin/prog;args"
    static void ParseApp(byte* line, AppEntry* a)
    {
        int field = 0, at = 0;
        for (byte* p = line; *p != 0; p++) {
            if (*p == ';') {
                field++;
                at = 0;
                continue;
            }
            if (*p == '\r')
                continue;
            if (field == 0 && at < 39)
                a->Name[at++] = *p;
            else if (field == 1 && at < 63)
                a->Path[at++] = *p;
            else if (field == 2 && at < 63)
                a->Args[at++] = *p;
        }
        if (a->Name[0] != 0 && a->Path[0] != 0)
            AppCount++;
        else
            *a = default;
    }
}
