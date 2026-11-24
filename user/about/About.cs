// about — "About Nekotia": the logo, the version, the machine and the graphics stack it is running on.
using Nekotia;
using Nekotia.Gui;
using Nekotia.User;

namespace AboutApp;

public static unsafe class Program
{
    static Window* _win;
    static Font* _font;
    static bool _hover;

    public static int Main()
    {
        if (App.Connect() < 0) {
            Console.WriteLine("about: no desktop is running (start one with `nekowm`)");
            return 1;
        }
        _font = App.SystemFont;
        _win = Memory.New<Window>();
        if (App.CreateWindow("About Nekotia", 480, 360, Protocol.FlagFixedSize | Protocol.FlagCentered, out *_win) < 0)
            return 1;
        Draw();
        while (true) {
            Message m = default;
            if (App.Next(out m, -1) < 0 || m.Type == Protocol.Close)
                break;
            if (m.Type == Protocol.Key && m.Value != 0 && (m.Codepoint == '\n' || m.Code == Keys.Escape))
                break;
            if (m.Type == Protocol.Pointer) {
                bool over = OkButton().Contains(m.X, m.Y) && m.Code != Protocol.PointerLeave;
                if (m.Code == Protocol.PointerButton && m.Value != 0 && over)
                    break;
                if (over != _hover) {
                    _hover = over;
                    Draw();
                }
            }
        }
        App.Destroy(ref *_win);
        return 0;
    }

    static Rect OkButton() => new Rect(_win->Width - 110, _win->Height - 46, 96, 32);

    /// First line of a text file into `out`.
    static void FirstLine(string path, byte* dst, int capacity)
    {
        dst[0] = 0;
        long fd = File.Open(path, File.Read);
        if (fd < 0)
            return;
        long n = File.ReadBytes((int)fd, dst, (ulong)(capacity - 1));
        File.Close((int)fd);
        if (n < 0)
            n = 0;
        dst[n] = 0;
        for (long i = 0; i < n; i++) {
            if (dst[i] == '\n') {
                dst[i] = 0;
                break;
            }
        }
    }

    static void Draw()
    {
        Canvas* c = &_win->Canvas;
        int w = _win->Width, h = _win->Height;
        c->GradientV(new Rect(0, 0, w, h), 0x2C2547, 0x1B1827);

        // the logo: a big cat face
        double cx = 92, cy = 112, s = 46;
        c->FillCircle(cx, cy + 6, s + 18, Theme.Accent, 18);
        c->FillTriangle((int)(cx - s), (int)(cy - s / 3), (int)(cx - s / 3), (int)(cy - s * 1.6), (int)(cx - s / 6), (int)(cy - s + 2), Theme.Accent);
        c->FillTriangle((int)(cx + s), (int)(cy - s / 3), (int)(cx + s / 3), (int)(cy - s * 1.6), (int)(cx + s / 6), (int)(cy - s + 2), Theme.Accent);
        c->FillCircle(cx, cy, s, Theme.Accent);
        c->FillCircle(cx - s * 0.38, cy - s * 0.08, 6, 0x1B1827);
        c->FillCircle(cx + s * 0.38, cy - s * 0.08, 6, 0x1B1827);
        c->FillCircle(cx - s * 0.38 + 2, cy - s * 0.08 - 2, 2, 0xFFFFFF);
        c->FillCircle(cx + s * 0.38 + 2, cy - s * 0.08 - 2, 2, 0xFFFFFF);
        c->FillTriangle((int)(cx - 5), (int)(cy + 10), (int)(cx + 5), (int)(cy + 10), (int)cx, (int)(cy + 16), 0x8A3E6A);
        c->LineAA(cx - s * 0.9, cy + 8, cx - s * 0.45, cy + 12, 1.2, 0x8A3E6A);
        c->LineAA(cx + s * 0.9, cy + 8, cx + s * 0.45, cy + 12, 1.2, 0x8A3E6A);

        int x = 176;
        _font->Draw(c, x, 54, "Nekotia", Theme.Text, 3);
        byte* line = stackalloc byte[160];
        FirstLine("/proc/version", line, 160);
        // "Nekotia 0.4.0 — …": show the version part
        byte* version = line;
        while (*version != 0 && *version != ' ')
            version++;
        while (*version == ' ')
            version++;
        byte* end = version;
        while (*end != 0 && *end != ' ')
            end++;
        *end = 0;
        byte* buf = stackalloc byte[96];
        Text t = new Text(buf, 96);
        t.Write("version ");
        t.Write(version);
        _font->Draw(c, x + 2, 110, buf, Theme.Accent);
        _font->Draw(c, x + 2, 132, "a kernel in C#, translated to C++", Theme.TextMuted);

        int y = 186;
        Row(c, y, "Processor", "/proc/cpuinfo", "model name:");
        Row(c, y + 24, "Processors", "/proc/cpuinfo", "processors:");
        Row(c, y + 48, "Memory", "/proc/meminfo", "MemTotal:");
        Row(c, y + 72, "Display", "/proc/display", "fb0 ");
        _font->Draw(c, 24, y + 96, "Desktop", Theme.TextMuted);
        _font->Draw(c, 130, y + 96, "nekowm · CPU compositor", Theme.Text);

        Theme.Button(c, _font, OkButton(), "OK", _hover ? 1 : 3);
        App.Present(ref *_win);
    }

    /// "label   value" where value is what follows `key` on its line of `path`.
    static void Row(Canvas* c, int y, string label, string path, string key)
    {
        _font->Draw(c, 24, y, label, Theme.TextMuted);
        byte* text = stackalloc byte[1024];
        text[0] = 0;
        long fd = File.Open(path, File.Read);
        if (fd >= 0) {
            long n = File.ReadBytes((int)fd, text, 1023);
            File.Close((int)fd);
            text[n < 0 ? 0 : n] = 0;
        }
        byte* k = Str.Bytes(key);
        int klen = (int)Str.Length(k);
        for (byte* p = text; *p != 0;) {
            bool match = true;
            for (int i = 0; i < klen; i++) {
                if (p[i] != k[i]) {
                    match = false;
                    break;
                }
            }
            byte* eol = p;
            while (*eol != 0 && *eol != '\n')
                eol++;
            if (match) {
                byte* v = p + klen;
                while (*v == ' ')
                    v++;
                byte saved = *eol;
                *eol = 0;
                _font->DrawClipped(c, 130, y, v, Theme.Text, _win->Width - 150);
                *eol = saved;
                return;
            }
            p = *eol == 0 ? eol : eol + 1;
        }
        _font->Draw(c, 130, y, "—", Theme.Text);
    }
}
