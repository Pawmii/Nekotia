// sysmon — the system monitor: CPU load with a one-minute graph, memory, and the processes, refreshed every
// second from /proc/stat, /proc/meminfo and the process table. Click a process and press Delete (or the
// button) to end it.
using Nekotia;
using Nekotia.Gui;
using Nekotia.User;

namespace SysMon;

public unsafe struct Samples
{
    public fixed int Load[120];   // permille, oldest first
}

public static unsafe class Program
{
    const int Pad = 14;
    const int RowHeight = 20;

    static Window* _win;
    static Font* _font;
    static Samples _history;
    static int _samples;
    static ulong _lastUptime, _lastIdle;
    static int _cpus = 1;
    static int _selected = -1;     // pid
    static int _hoverRow = -1;
    static bool _hoverKill;
    static int _listTop;           // y of the first process row

    public static int Main()
    {
        if (App.Connect() < 0) {
            Console.WriteLine("sysmon: no desktop is running (start one with `nekowm`)");
            return 1;
        }
        _font = App.SystemFont;
        _win = Memory.New<Window>();
        if (App.CreateWindow("System Monitor", 520, 470, 0, out *_win) < 0)
            return 1;
        Sample();
        Draw();
        ulong next = Time.UptimeMs() + 1000;
        while (true) {
            ulong now = Time.UptimeMs();
            if (now >= next) {
                Sample();
                Draw();
                next = now + 1000;
            }
            Message m = default;
            int r = App.Next(out m, (long)(next - Time.UptimeMs() > 1000 ? 1000 : next - Time.UptimeMs()));
            if (r < 0)
                break;
            if (r == 0)
                continue;
            if (m.Type == Protocol.Close)
                break;
            if (m.Type == Protocol.Configure) {
                App.Resize(ref *_win, m.Width < 380 ? 380 : m.Width, m.Height < 320 ? 320 : m.Height);
                Draw();
            } else if (m.Type == Protocol.Pointer) {
                OnPointer(&m);
            } else if (m.Type == Protocol.Key && m.Value != 0 && m.Code == Keys.Delete) {
                KillSelected();
            }
        }
        App.Destroy(ref *_win);
        return 0;
    }

    // ---------------------------------------------------------------- data

    /// The number after `key` in a /proc text file, -1 when it is not there.
    static long ProcNumber(string path, string key)
    {
        byte* text = stackalloc byte[2048];
        long fd = File.Open(path, File.Read);
        if (fd < 0)
            return -1;
        long n = File.ReadBytes((int)fd, text, 2047);
        File.Close((int)fd);
        if (n <= 0)
            return -1;
        text[n] = 0;
        byte* k = Str.Bytes(key);
        int klen = (int)Str.Length(k);
        for (byte* line = text; *line != 0;) {
            bool match = true;
            for (int i = 0; i < klen; i++) {
                if (line[i] != k[i]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                byte* p = line + klen;
                while (*p == ' ' || *p == ':')
                    p++;
                long v = 0;
                while (*p >= '0' && *p <= '9') {
                    v = v * 10 + (*p - '0');
                    p++;
                }
                return v;
            }
            while (*line != 0 && *line != '\n')
                line++;
            if (*line == '\n')
                line++;
        }
        return -1;
    }

    static void Sample()
    {
        long cpus = ProcNumber("/proc/stat", "cpus");
        long up = ProcNumber("/proc/stat", "uptime_ms");
        long idle = ProcNumber("/proc/stat", "idle_ms");
        if (cpus > 0)
            _cpus = (int)cpus;
        if (up < 0 || idle < 0)
            return;
        if (_lastUptime != 0 && (ulong)up > _lastUptime) {
            ulong span = ((ulong)up - _lastUptime) * (ulong)_cpus;
            ulong idleSpan = (ulong)idle >= _lastIdle ? (ulong)idle - _lastIdle : 0;
            int load = idleSpan >= span ? 0 : (int)(1000 - idleSpan * 1000 / span);
            if (_samples == 120) {
                for (int i = 1; i < 120; i++)
                    _history.Load[i - 1] = _history.Load[i];
                _samples = 119;
            }
            _history.Load[_samples++] = load;
        }
        _lastUptime = (ulong)up;
        _lastIdle = (ulong)idle;
    }

    // ---------------------------------------------------------------- drawing

    static void Draw()
    {
        Canvas* c = &_win->Canvas;
        int w = _win->Width, h = _win->Height;
        c->Clear(Theme.Window);
        int y = Pad;

        // CPU
        int load = _samples > 0 ? _history.Load[_samples - 1] : 0;
        byte* line = stackalloc byte[96];
        Text t = new Text(line, 96);
        t.Write($"CPU  {load / 10}.{load % 10}%  ·  {_cpus} cores");
        Theme.Heading(c, _font, Pad, y, w - 2 * Pad, line);
        y += 24;
        Rect graph = new Rect(Pad, y, w - 2 * Pad, 90);
        c->FillRoundRect(graph, 6, 0x15131E);
        for (int g = 1; g < 4; g++)
            c->HLine(graph.X + 4, graph.Y + graph.H * g / 4, graph.W - 8, 0x24202F);
        if (_samples > 1) {
            double step = (graph.W - 8) / 119.0;
            double x0 = graph.Right - 4 - (_samples - 1) * step;
            for (int i = 0; i < _samples; i++) {
                double x = x0 + i * step;
                double yv = graph.Bottom - 4 - _history.Load[i] * (graph.H - 8) / 1000.0;
                // area under the line
                c->FillRectAlpha(new Rect((int)x, (int)yv, (int)step + 1, graph.Bottom - 4 - (int)yv), Theme.Accent2, 40);
                if (i > 0) {
                    double px = x0 + (i - 1) * step;
                    double py = graph.Bottom - 4 - _history.Load[i - 1] * (graph.H - 8) / 1000.0;
                    c->LineAA(px, py, x, yv, 1.8, Theme.Accent2);
                }
            }
        }
        y += graph.H + 16;

        // memory
        long total = ProcNumber("/proc/meminfo", "MemTotal");
        long free = ProcNumber("/proc/meminfo", "MemFree");
        long heap = ProcNumber("/proc/meminfo", "HeapUsed");
        t = new Text(line, 96);
        if (total > 0 && free >= 0) {
            long used = total - free;
            t.Write($"Memory  {used / 1024} / {total / 1024} MiB");
            Theme.Heading(c, _font, Pad, y, w - 2 * Pad, line);
            y += 24;
            Theme.Bar(c, new Rect(Pad, y, w - 2 * Pad, 12), (int)(used * 1000 / total), Theme.Accent);
            y += 18;
            t = new Text(line, 96);
            t.Write($"kernel heap {heap} KiB");
            _font->Draw(c, Pad, y, line, Theme.TextMuted);
            y += 26;
        }

        // processes
        ProcessInfo p = default;
        int count = 0;
        for (uint i = 0; Process.GetInfo(i, out p); i++)
            count++;
        t = new Text(line, 96);
        t.Write($"Processes  {count}");
        Theme.Heading(c, _font, Pad, y, w - 2 * Pad, line);
        y += 24;
        _font->Draw(c, Pad + 6, y, "PID", Theme.TextMuted);
        _font->Draw(c, Pad + 56, y, "NAME", Theme.TextMuted);
        _font->Draw(c, w - Pad - 210, y, "THREADS", Theme.TextMuted);
        _font->Draw(c, w - Pad - 120, y, "MEMORY", Theme.TextMuted);
        y += 20;
        _listTop = y;
        int row = 0;
        int bottom = h - Pad - 40;
        for (uint i = 0; Process.GetInfo(i, out p) && y + RowHeight <= bottom; i++) {
            Rect r = new Rect(Pad, y, w - 2 * Pad, RowHeight);
            if ((int)p.Pid == _selected)
                c->FillRoundRect(r, 4, 0x3A2F5C);
            else if (row == _hoverRow)
                c->FillRoundRect(r, 4, Theme.Surface);
            int ty = y + (RowHeight - _font->Height) / 2;
            t = new Text(line, 96);
            t.WriteDec((long)p.Pid);
            _font->Draw(c, Pad + 6, ty, line, Theme.Text);
            _font->DrawClipped(c, Pad + 56, ty, p.Name, p.State == 0 ? Theme.Text : Theme.TextMuted, w - Pad * 2 - 56 - 220);
            t = new Text(line, 96);
            t.WriteDec((long)p.Threads);
            _font->Draw(c, w - Pad - 210, ty, line, Theme.TextMuted);
            t = new Text(line, 96);
            t.Write($"{p.MemoryKib} KiB");
            _font->Draw(c, w - Pad - 120, ty, line, Theme.TextMuted);
            y += RowHeight;
            row++;
        }

        Theme.Button(c, _font, KillButton(), "End process", _selected < 0 ? 0 : _hoverKill ? 1 : 3);
        App.Present(ref *_win);
    }

    static Rect KillButton() => new Rect(_win->Width - Pad - 130, _win->Height - Pad - 30, 130, 30);

    // ---------------------------------------------------------------- input

    static void OnPointer(Message* m)
    {
        int row = m->Y >= _listTop ? (m->Y - _listTop) / RowHeight : -1;
        bool kill = KillButton().Contains(m->X, m->Y);
        if (m->Code == Protocol.PointerLeave) {
            row = -1;
            kill = false;
        }
        if (m->Code == Protocol.PointerButton && m->Value != 0) {
            if (kill) {
                KillSelected();
                return;
            }
            ProcessInfo p = default;
            _selected = row >= 0 && Process.GetInfo((uint)row, out p) ? (int)p.Pid : -1;
            Draw();
            return;
        }
        if (row != _hoverRow || kill != _hoverKill) {
            _hoverRow = row;
            _hoverKill = kill;
            Draw();
        }
    }

    static void KillSelected()
    {
        if (_selected <= 0 || (uint)_selected == Process.Id())
            return;
        Process.Kill((uint)_selected);
        _selected = -1;
        Process.Sleep(50);
        Draw();
    }
}
