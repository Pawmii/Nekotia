// clock — an analog clock for the desktop: anti-aliased face and hands, the date underneath. It redraws
// once a second (App.Next with a timeout) and adapts to whatever size the desktop gives it.
using Nekotia;
using Nekotia.Gui;
using Nekotia.User;

namespace ClockApp;

public static unsafe class Program
{
    static Window* _win;
    static Font* _font;

    public static int Main()
    {
        if (App.Connect() < 0) {
            Console.WriteLine("clock: no desktop is running (start one with `nekowm`)");
            return 1;
        }
        _font = App.SystemFont;
        _win = Memory.New<Window>();
        if (App.CreateWindow("Clock", 260, 300, 0, out *_win) < 0)
            return 1;
        long shown = -1;
        while (true) {
            long now = Clock.Now();
            if (now == 0)
                now = (long)(Time.UptimeMs() / 1000);
            if (now != shown) {
                shown = now;
                Draw(now);
                App.Present(ref *_win);
            }
            Message m = default;
            int r = App.Next(out m, 1000 - (long)(Time.UptimeMs() % 1000));
            if (r < 0)
                break;
            if (r == 0)
                continue;
            if (m.Type == Protocol.Close)
                break;
            if (m.Type == Protocol.Configure) {
                int w = m.Width < 160 ? 160 : m.Width;
                int h = m.Height < 200 ? 200 : m.Height;
                App.Resize(ref *_win, w, h);
                shown = -1;
            }
        }
        App.Destroy(ref *_win);
        return 0;
    }

    static void Draw(long now)
    {
        Canvas* c = &_win->Canvas;
        int w = _win->Width, h = _win->Height;
        c->GradientV(new Rect(0, 0, w, h), 0x241F36, 0x17141F);

        int footer = 52;
        double size = w < h - footer ? w : h - footer;
        double r = size / 2 - 16;
        double cx = w / 2.0, cy = (h - footer) / 2.0 + 4;

        // face
        c->FillCircle(cx, cy + 3, r + 6, 0x000000, 60);
        c->FillCircle(cx, cy, r + 4, 0x3A3354);
        c->FillCircle(cx, cy, r, 0x1B1827);
        c->DrawCircle(cx, cy, r - 1, 1.5, 0x4A4266);
        for (int i = 0; i < 60; i++) {
            double a = i * Maths.Pi / 30;
            double s = Maths.Sin(a), co = Maths.Cos(a);
            bool hour = i % 5 == 0;
            double inner = r - (hour ? 14 : 7);
            c->LineAA(cx + s * inner, cy - co * inner, cx + s * (r - 4), cy - co * (r - 4), hour ? 2.6 : 1.1,
                hour ? Theme.Text : Theme.TextMuted);
        }
        for (int n = 1; n <= 12; n++) {
            double a = n * Maths.Pi / 6;
            double tr = r - 28;
            byte* digits = stackalloc byte[4];
            Text t = new Text(digits, 4);
            t.WriteDec((long)n);
            int tw = _font->Measure(digits);
            _font->Draw(c, (int)(cx + Maths.Sin(a) * tr) - tw / 2, (int)(cy - Maths.Cos(a) * tr) - _font->Height / 2, digits, Theme.TextMuted);
        }

        DateTime d = Clock.ToDate(now);
        double sec = d.Second, min = d.Minute + sec / 60, hr = d.Hour % 12 + min / 60;
        Hand(c, cx, cy, hr * Maths.Pi / 6, r * 0.52, 5, Theme.Text);
        Hand(c, cx, cy, min * Maths.Pi / 30, r * 0.78, 3.4, Theme.Text);
        Hand(c, cx, cy, sec * Maths.Pi / 30, r * 0.86, 1.4, Theme.Accent);
        c->FillCircle(cx, cy, 5, Theme.Accent);
        c->FillCircle(cx, cy, 2, 0x1B1827);

        // date and digital time
        byte* line = stackalloc byte[64];
        Text tx = new Text(line, 64);
        tx.Write($"{d.Hour:D2}:{d.Minute:D2}:{d.Second:D2}");
        _font->DrawCentered(c, new Rect(0, h - footer, w, 24), line, Theme.Text, 1);
        tx = new Text(line, 64);
        if (d.Year > 1970) {
            tx.Write(Clock.DayName(d.WeekDay));
            tx.Write($", {d.Day} ");
            tx.Write(Clock.MonthName(d.Month));
            tx.Write($" {d.Year}");
        } else {
            tx.Write("no real-time clock: uptime");
        }
        _font->DrawCentered(c, new Rect(0, h - footer + 22, w, 22), line, Theme.TextMuted, 1);
    }

    static void Hand(Canvas* c, double cx, double cy, double angle, double length, double width, uint color)
    {
        double s = Maths.Sin(angle), co = Maths.Cos(angle);
        c->LineAA(cx - s * length * 0.12 + 1, cy + co * length * 0.12 + 2, cx + s * length + 1, cy - co * length + 2, width, 0, 90);   // shadow
        c->LineAA(cx - s * length * 0.12, cy + co * length * 0.12, cx + s * length, cy - co * length, width, color);
    }
}
