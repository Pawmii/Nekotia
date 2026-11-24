// The compositor: every damaged rectangle is repainted bottom-up into the back buffer — wallpaper, each
// window (shadow, frame, content), the resize outline, the panel, the launcher menu and the cursor — and
// then copied to the screen. Translucency (shadows, the panel) falls out of the bottom-up order for free.
using Nekotia;
using Nekotia.Gui;
using Nekotia.User;

namespace NekoWm;

public static unsafe class Render
{
    const uint TitleFocusedTop = 0x3B2F5E, TitleFocusedBottom = 0x2C2547, TitleIdle = 0x23202F;
    const uint FrameColor = 0x4A4266, FrameIdle = 0x332F42;
    const uint PanelColor = 0x12101B;
    const int MenuWidth = 270, MenuHeader = 54, MenuItem = 32;

    // ---------------------------------------------------------------- layout

    public static Rect PanelRect() => new Rect(0, Desktop.Height - Desktop.PanelHeight, Desktop.Width, Desktop.PanelHeight);

    public static Rect LauncherButtonRect() => new Rect(6, Desktop.Height - Desktop.PanelHeight + 4, 96, Desktop.PanelHeight - 8);

    public static Rect ClockRect() => new Rect(Desktop.Width - 186, Desktop.Height - Desktop.PanelHeight, 186, Desktop.PanelHeight);

    static int TaskCount()
    {
        int n = 0;
        for (int i = 0; i < WindowServer.MaxWindows; i++) {
            ServerWindow* w = WindowServer.Get(i);
            if (w->Used && !w->Dying && w->Visible)
                n++;
        }
        return n;
    }

    /// Task button number `slot` (left to right).
    static Rect TaskRect(int slot, int count)
    {
        int left = 112, right = Desktop.Width - 196;
        int width = count > 0 ? (right - left) / count : 0;
        if (width > 190)
            width = 190;
        return new Rect(left + slot * width, Desktop.Height - Desktop.PanelHeight + 5, width - 6, Desktop.PanelHeight - 10);
    }

    public static int TaskAt(int x, int y)
    {
        int count = TaskCount(), slot = 0;
        for (int i = 0; i < WindowServer.MaxWindows; i++) {
            ServerWindow* w = WindowServer.Get(i);
            if (!w->Used || w->Dying || !w->Visible)
                continue;
            if (TaskRect(slot, count).Contains(x, y))
                return i;
            slot++;
        }
        return -1;
    }

    public static Rect MenuRect()
    {
        int h = MenuHeader + (Desktop.AppCount + 1) * MenuItem + 17;
        return new Rect(6, Desktop.Height - Desktop.PanelHeight - 8 - h, MenuWidth, h);
    }

    static Rect MenuItemRect(int item)
    {
        Rect m = MenuRect();
        int y = m.Y + MenuHeader + item * MenuItem + (item >= Desktop.AppCount ? 9 : 0);
        return new Rect(m.X + 6, y, m.W - 12, MenuItem - 2);
    }

    public static int MenuItemAt(int x, int y)
    {
        for (int i = 0; i <= Desktop.AppCount; i++) {
            if (MenuItemRect(i).Contains(x, y))
                return i;
        }
        return -1;
    }

    // ---------------------------------------------------------------- composing

    // Profiling (nekowm -v): cycles per phase, summed until Report.
    public static ulong Frames, Pixels, CyWall, CyShadow, CyDecor, CyContent, CyPanel, CyPresent;

    public static void Compose()
    {
        Canvas* b = Desktop.Back;
        Frames++;
        for (int i = 0; i < Desktop.Dirty.Count; i++) {
            Rect r = Desktop.Dirty.Get(i);
            Pixels += (ulong)r.Area;
            ComposeRect(b, r);
        }
        ulong t = Time.Cycles();
        fixed (Damage* d = &Desktop.Dirty) {
            Desktop.Output->PresentFrame(d);   // flip to the new frame, or copy it to the screen
        }
        CyPresent += Time.Cycles() - t;
        Desktop.Dirty.Clear();
    }

    public static void Report(ulong mhz, ulong ms)
    {
        if (Frames == 0)
            return;
        ulong k = mhz * 1000;   // cycles per ms
        Console.WriteLine($"nekowm: {Frames * 1000 / ms} fps, {Pixels / Frames} px/frame; ms per frame: wallpaper {CyWall / Frames * 100 / k / 100.0:F2} shadow {CyShadow * 100 / Frames / k / 100.0:F2} decor {CyDecor * 100 / Frames / k / 100.0:F2} content {CyContent * 100 / Frames / k / 100.0:F2} panel {CyPanel * 100 / Frames / k / 100.0:F2} present {CyPresent * 100 / Frames / k / 100.0:F2}");
        Frames = Pixels = CyWall = CyShadow = CyDecor = CyContent = CyPanel = CyPresent = 0;
    }

    static void ComposeRect(Canvas* b, Rect r)
    {
        b->SetClip(r);
        ulong t0 = Time.Cycles();
        b->Blit(Desktop.Wallpaper, r, r.X, r.Y);
        CyWall += Time.Cycles() - t0;
        for (int k = 0; k < Desktop.StackCount; k++) {
            int i = Desktop.Stack[k];
            ServerWindow* w = WindowServer.Get(i);
            if (Desktop.Shown(w) && Desktop.Extent(w).Overlaps(r))
                DrawWindow(b, i, w);
        }
        if (Desktop.Resizing) {
            Rect o = Desktop.ResizeOutline;
            b->FillRectAlpha(o, Theme.Accent, 28);
            b->DrawRect(o, Theme.Accent);
            b->DrawRect(o.Inflate(-1), Theme.AccentDark);
        }
        t0 = Time.Cycles();
        if (PanelRect().Overlaps(r))
            DrawPanel(b);
        if (Desktop.MenuOpen && MenuRect().Inflate(Desktop.ShadowSize).Overlaps(r))
            DrawMenu(b);
        Cursor.Draw(b, Desktop.CursorShape, Desktop.PointerX, Desktop.PointerY);
        CyPanel += Time.Cycles() - t0;
        b->ResetClip();
    }

    static void DrawWindow(Canvas* b, int index, ServerWindow* w)
    {
        bool focused = index == Desktop.Focus;
        Rect f = Desktop.Frame(w);
        ulong t0 = Time.Cycles();
        b->Shadow(f, Desktop.ShadowSize, focused ? 120u : 70u, Desktop.ShadowOffset);
        ulong t1 = Time.Cycles();
        CyShadow += t1 - t0;
        if (Desktop.Decorated(w)) {
            b->FillRoundRect(f, 9, focused ? FrameColor : FrameIdle, 3);
            Rect title = new Rect(f.X + 1, f.Y + 1, f.W - 2, Desktop.TitleHeight);
            if (title.Overlaps(b->Clip)) {
                if (focused) {
                    b->FillRoundRect(title, 8, TitleFocusedBottom, 3);
                    b->GradientV(new Rect(title.X + 8, title.Y, title.W - 16, title.H), TitleFocusedTop, TitleFocusedBottom);
                    b->GradientV(new Rect(title.X, title.Y + 8, title.W, title.H - 8),
                        Color.Mix(TitleFocusedTop, TitleFocusedBottom, 8 * 256 / title.H), TitleFocusedBottom);
                } else {
                    b->FillRoundRect(title, 8, TitleIdle, 3);
                }
                // a small cat as the window icon, then the title
                CatIcon(b, title.X + 18, title.Y + title.H / 2 + 1, 6, focused ? Theme.Accent : Theme.TextMuted);
                Font* font = Desktop.UiFont;
                font->DrawClipped(b, title.X + 32, title.Y + (title.H - font->Height) / 2, w->Title,
                    focused ? Theme.Text : Theme.TextMuted, title.W - 32 - 84);
                DrawButtons(b, index, w, focused);
            }
        }
        ulong t2 = Time.Cycles();
        CyDecor += t2 - t1;
        Canvas content = new Canvas(w->Pixels, w->Width, w->Height, w->Width);
        b->Blit(&content, content.Bounds, w->X, w->Y);
        CyContent += Time.Cycles() - t2;
    }

    static void DrawButtons(Canvas* b, int index, ServerWindow* w, bool focused)
    {
        for (int which = 0; which < 3; which++) {
            if (which == 1 && (w->Flags & Protocol.FlagFixedSize) != 0)
                continue;
            Point c = Desktop.ButtonCentre(w, which);
            int area = which == 0 ? Desktop.AreaClose : which == 1 ? Desktop.AreaMaximize : Desktop.AreaMinimize;
            bool hover = Desktop.HoverArea == area && Desktop.HoverIndex == index;
            uint color = which == 0 ? 0xF2567Au : which == 1 ? 0xFFC857u : 0x7BD88Fu;
            if (!focused && !hover)
                color = 0x4A4560;
            b->FillCircle(c.X, c.Y, 6.5, color);
            if (!hover)
                continue;
            uint ink = 0x2A1020;
            if (which == 0) {
                b->LineAA(c.X - 2.5, c.Y - 2.5, c.X + 2.5, c.Y + 2.5, 1.6, ink);
                b->LineAA(c.X - 2.5, c.Y + 2.5, c.X + 2.5, c.Y - 2.5, 1.6, ink);
            } else if (which == 1) {
                b->DrawRect(new Rect(c.X - 3, c.Y - 3, 7, 7), ink);
            } else {
                b->FillRect(new Rect(c.X - 3, c.Y, 7, 2), ink);
            }
        }
    }

    // ---------------------------------------------------------------- panel

    static void DrawPanel(Canvas* b)
    {
        Rect p = PanelRect();
        b->FillRectAlpha(p, PanelColor, 222);
        b->FillRectAlpha(new Rect(p.X, p.Y, p.W, 1), 0xFFFFFF, 26);
        Font* font = Desktop.UiFont;

        // launcher button
        Rect lb = LauncherButtonRect();
        bool hot = Desktop.MenuOpen || Desktop.HoverArea == Desktop.AreaLauncherButton;
        if (hot)
            b->FillRoundRect(lb, 7, Desktop.MenuOpen ? 0x4A3760u : Theme.SurfaceHover);
        CatIcon(b, lb.X + 20, lb.Y + lb.H / 2 + 2, 8, Theme.Accent);
        font->Draw(b, lb.X + 38, lb.Y + (lb.H - font->Height) / 2, "Neko", Theme.Text);
        b->FillRect(new Rect(lb.Right + 6, p.Y + 9, 1, p.H - 18), 0x3D3756);

        // tasks
        int count = TaskCount(), slot = 0;
        for (int i = 0; i < WindowServer.MaxWindows; i++) {
            ServerWindow* w = WindowServer.Get(i);
            if (!w->Used || w->Dying || !w->Visible)
                continue;
            Rect t = TaskRect(slot++, count);
            if (t.W < 24)
                continue;
            bool focused = i == Desktop.Focus && !w->Minimized;
            bool hover = Desktop.HoverArea == Desktop.AreaTask && Desktop.HoverIndex == i;
            if (focused)
                b->FillRoundRect(t, 6, 0x3A2F5C);
            else if (hover)
                b->FillRoundRect(t, 6, Theme.SurfaceHover);
            else
                b->FillRoundRect(t, 6, 0x1D1A2A);
            if (focused)
                b->FillRoundRect(new Rect(t.X + t.W / 2 - 14, t.Bottom - 3, 28, 3), 1, Theme.Accent);
            CatIcon(b, t.X + 14, t.Y + t.H / 2 + 1, 5, w->Minimized ? Theme.TextMuted : Theme.Accent2);
            font->DrawClipped(b, t.X + 26, t.Y + (t.H - font->Height) / 2, w->Title,
                w->Minimized ? Theme.TextMuted : Theme.Text, t.W - 32);
        }

        // clock
        Rect c = ClockRect();
        DateTime now = Clock.Today();
        byte* text = stackalloc byte[48];
        Text tx = new Text(text, 48);
        if (now.Year > 1970) {
            tx.Write(DayShort(now.WeekDay));
            tx.Write($" {now.Day} ");
            tx.Write(MonthShort(now.Month));
            tx.Write($"  {now.Hour:D2}:{now.Minute:D2}");
        } else {
            ulong s = Time.UptimeMs() / 1000;
            tx.Write($"up {s / 3600}:{s / 60 % 60:D2}");
        }
        int tw = font->Measure(text);
        font->Draw(b, c.Right - tw - 14, c.Y + (c.H - font->Height) / 2, text, Theme.Text);
    }

    static string DayShort(int d) => d switch
    {
        0 => "Mon", 1 => "Tue", 2 => "Wed", 3 => "Thu", 4 => "Fri", 5 => "Sat", _ => "Sun",
    };

    static string MonthShort(int m) => m switch
    {
        1 => "Jan", 2 => "Feb", 3 => "Mar", 4 => "Apr", 5 => "May", 6 => "Jun",
        7 => "Jul", 8 => "Aug", 9 => "Sep", 10 => "Oct", 11 => "Nov", _ => "Dec",
    };

    // ---------------------------------------------------------------- launcher menu

    static void DrawMenu(Canvas* b)
    {
        Rect m = MenuRect();
        Font* font = Desktop.UiFont;
        b->Shadow(m, Desktop.ShadowSize, 130, 4);
        b->FillRoundRect(m, 10, 0x4A4266);
        b->FillRoundRect(m.Inflate(-1), 9, 0x1B1827);
        // header
        Rect h = new Rect(m.X + 1, m.Y + 1, m.W - 2, MenuHeader - 8);
        b->FillRoundRect(h, 9, 0x2C2547, 3);
        CatIcon(b, h.X + 26, h.Y + h.H / 2 + 3, 12, Theme.Accent);
        font->Draw(b, h.X + 50, h.Y + 6, "Nekotia", Theme.Text, 2);
        font->Draw(b, h.X + 50 + 7 * 16 + 8, h.Y + 21, "desktop", Theme.TextMuted);

        for (int i = 0; i <= Desktop.AppCount; i++) {
            Rect r = MenuItemRect(i);
            bool hover = Desktop.HoverArea == Desktop.AreaMenu && Desktop.HoverIndex == i;
            if (hover)
                b->FillRoundRect(r, 6, 0x3A2F5C);
            if (i == Desktop.AppCount) {
                b->FillRect(new Rect(m.X + 12, r.Y - 5, m.W - 24, 1), 0x3D3756);
                b->FillCircle(r.X + 16, r.Y + r.H / 2, 5, Theme.Bad);
                font->Draw(b, r.X + 32, r.Y + (r.H - font->Height) / 2, "Exit desktop", Theme.Text);
                continue;
            }
            uint dot = AppColor(i);
            b->FillRoundRect(new Rect(r.X + 8, r.Y + 7, 16, 16), 4, dot);
            font->Draw(b, r.X + 32, r.Y + (r.H - font->Height) / 2, Desktop.Apps[i].Name, Theme.Text);
        }
    }

    static uint AppColor(int i) => (i % 5) switch
    {
        0 => 0x8FB8FFu,
        1 => 0x7BD88Fu,
        2 => 0xFFC857u,
        3 => 0xF5A3C7u,
        _ => 0xC6A0F6u,
    };

    // ---------------------------------------------------------------- drawing helpers

    /// A cat face of radius `s` centred at (cx, cy): head, ears, eyes.
    public static void CatIcon(Canvas* b, int cx, int cy, int s, uint color)
    {
        b->FillTriangle(cx - s, cy - s / 3, cx - s + s * 2 / 3, cy - s - s * 2 / 3, cx - s / 6, cy - s + 1, color);
        b->FillTriangle(cx + s, cy - s / 3, cx + s - s * 2 / 3, cy - s - s * 2 / 3, cx + s / 6, cy - s + 1, color);
        b->FillCircle(cx, cy, s, color);
        if (s >= 5) {
            uint eye = 0x15131E;
            double er = s >= 10 ? 1.6 : 1.0;
            b->FillCircle(cx - s * 0.38, cy - s * 0.1, er, eye);
            b->FillCircle(cx + s * 0.38, cy - s * 0.1, er, eye);
        }
    }

    // ---------------------------------------------------------------- wallpaper

    static uint _seed = 0x9E3779B9;

    static uint Random()
    {
        _seed ^= _seed << 13;
        _seed ^= _seed >> 17;
        _seed ^= _seed << 5;
        return _seed;
    }

    static int RandomIn(int min, int max) => min + (int)(Random() % (uint)(max - min + 1));

    /// Night sky, a moon, soft lights and a cat on a hill — painted once at start-up.
    public static void PaintWallpaper(Canvas* c)
    {
        int w = c->Width, h = c->Height;
        c->GradientV(new Rect(0, 0, w, h), 0x2A1B4A, 0x0A0E22);
        // a warm glow low on the left, a cool one top right
        for (int k = 0; k < 14; k++) {
            double t = 1 - k / 14.0;
            c->FillCircle(w * 0.18, h * 0.95, h * 0.75 * t, 0x8A3E7A, 5);
            c->FillCircle(w * 0.85, h * 0.15, h * 0.55 * t, 0x2D4A8A, 4);
        }
        // stars
        for (int i = 0; i < w * h / 6000; i++) {
            int x = RandomIn(0, w - 1), y = RandomIn(0, h * 2 / 3);
            uint a = (uint)RandomIn(50, 200);
            c->BlendPixel(x, y, 0xFFFFFF, a);
            if (a > 170) {
                c->BlendPixel(x + 1, y, 0xFFFFFF, a / 3);
                c->BlendPixel(x - 1, y, 0xFFFFFF, a / 3);
                c->BlendPixel(x, y + 1, 0xFFFFFF, a / 3);
                c->BlendPixel(x, y - 1, 0xFFFFFF, a / 3);
            }
        }
        // soft round lights
        for (int i = 0; i < 26; i++) {
            uint color = (i % 3) switch { 0 => 0xF5A3C7u, 1 => 0x8FB8FFu, _ => 0xC6A0F6u };
            c->FillCircle(RandomIn(0, w), RandomIn(h / 8, h - 60), RandomIn(6, 44), color, (uint)RandomIn(8, 22));
        }
        // the moon
        double mx = w * 0.80, my = h * 0.22, mr = h * 0.07;
        for (int k = 1; k <= 10; k++)
            c->FillCircle(mx, my, mr + k * mr * 0.25, 0xFFE9C4, 4);
        c->FillCircle(mx, my, mr, 0xFFF1D6);
        c->FillCircle(mx - mr * 0.3, my - mr * 0.2, mr * 0.18, 0xF0DDBC);
        c->FillCircle(mx + mr * 0.35, my + mr * 0.3, mr * 0.12, 0xF0DDBC);

        // hills
        uint hill = 0x0B0A1A;
        double baseY = h - Desktop.PanelHeight;
        c->FillCircle(w * 0.78, baseY + h * 0.62, h * 0.72, 0x120F26);
        c->FillCircle(w * 0.25, baseY + h * 0.75, h * 0.8, hill);
        // the cat, sitting on the hill and looking at the moon
        double cx = w * 0.70, cy = baseY - h * 0.105;
        double s = h * 0.055;
        c->FillCircle(cx, cy + s * 1.1, s * 1.25, hill);            // body
        c->FillCircle(cx + s * 0.2, cy + s * 1.9, s * 1.15, hill);  // haunch
        c->FillCircle(cx - s * 0.1, cy - s * 0.45, s * 0.8, hill);  // head
        c->FillTriangle((int)(cx - s * 0.85), (int)(cy - s * 0.7), (int)(cx - s * 0.55), (int)(cy - s * 1.6), (int)(cx - s * 0.15), (int)(cy - s * 1.0), hill);
        c->FillTriangle((int)(cx + s * 0.65), (int)(cy - s * 0.7), (int)(cx + s * 0.40), (int)(cy - s * 1.6), (int)(cx - s * 0.05), (int)(cy - s * 1.05), hill);
        // tail curling on the ground
        double tx = cx + s * 1.2, ty = cy + s * 2.6;
        for (int i = 0; i < 24; i++) {
            double a0 = i / 24.0 * 3.0, a1 = (i + 1) / 24.0 * 3.0;
            double x0 = tx + Maths.Cos(a0 - 1.2) * s * 0.9 + a0 * s * 0.3, y0 = ty - Maths.Sin(a0) * s * 0.6;
            double x1 = tx + Maths.Cos(a1 - 1.2) * s * 0.9 + a1 * s * 0.3, y1 = ty - Maths.Sin(a1) * s * 0.6;
            c->LineAA(x0, y0, x1, y1, s * 0.28, hill);
        }
        c->FillRect(new Rect(0, (int)baseY, w, h - (int)baseY), hill);

        // name
        Font* font = Desktop.UiFont;
        uint ink = Color.Mix(0x0B0A1A, 0xF5A3C7, 70);
        font->Draw(c, 28, (int)baseY - 70, "Nekotia", ink, 3);
        font->Draw(c, 30, (int)baseY - 20, "C# kernel · nekowm", Color.Mix(0x0B0A1A, 0xE6E3F2, 60));
    }
}
