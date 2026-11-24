// term — a terminal window for the desktop. It runs /bin/sh with its stdin and stdout/stderr on pipes, shows
// what comes out (UTF-8, ANSI colours, clear screen, cursor moves, erase to end of line) and edits the line
// being typed itself (the shell reads whole lines, like from the console): Backspace, ←/→, Home/End,
// ↑/↓ for the history, Ctrl+C ends what the shell is running, Ctrl+D ends the shell, Ctrl+L clears.
// The mouse selects text; Ctrl+Shift+C copies it, Ctrl+Shift+V or the middle button pastes.
using Nekotia;
using Nekotia.Gui;
using Nekotia.User;

namespace Term;

public struct Cell
{
    public uint Cp;
    public uint Fg;
    public uint Bg;
}

public unsafe struct LineBuffer
{
    public fixed uint Chars[256];
}

public unsafe struct History
{
    public fixed byte Lines[16 * 256];
}

public static unsafe class Program
{
    const int Pad = 6;
    const uint DefaultFg = 0xD8D6E8, DefaultBg = 0x15131E;
    const int MaxLine = 250;

    static Window* _win;
    static Font* _font;
    static Cell* _cells;
    static int _cols, _rows;
    static int _x, _y;
    static uint _fg = DefaultFg, _bg = DefaultBg;
    static bool _focused = true;
    static Rect _dirty;

    // the line being typed (codepoints), not yet sent to the shell
    static LineBuffer _line;
    static int _lineLength, _lineCursor;
    static int _lineX, _lineY;   // where it starts on screen
    static History* _history;
    static int _historyCount, _historyAt;

    // escape parser
    static int _esc;   // 0 none, 1 after ESC, 2 in CSI
    static int _p0, _p1, _params;
    static uint _utf8, _utf8Left;

    static int _shellIn = -1, _shellOut = -1;
    static uint _shell;

    public static int Main()
    {
        if (App.Connect() < 0) {
            Console.WriteLine("term: no desktop is running (start one with `nekowm`)");
            return 1;
        }
        _font = App.SystemFont;
        _cols = 88;
        _rows = 28;
        _win = Memory.New<Window>();
        if (App.CreateWindow("Terminal", _cols * _font->Width + 2 * Pad, _rows * _font->Height + 2 * Pad, 0, out *_win) < 0)
            return 1;
        _cells = Memory.NewArray<Cell>((ulong)(_cols * _rows));
        _history = Memory.New<History>();
        ClearCells(0, _cols * _rows);
        if (!StartShell()) {
            Put("term: cannot start /bin/sh\n");
        }
        RedrawAll();

        PollFd* fds = stackalloc PollFd[2];
        byte* buffer = stackalloc byte[4096];
        while (true) {
            fds[0].Fd = App.Fd;
            fds[0].Events = Poll.In;
            fds[0].Ready = 0;
            fds[1].Fd = _shellOut;
            fds[1].Events = Poll.In;
            fds[1].Ready = 0;
            // Messages that came in while Present waited are already ours: poll would not show them.
            if (Poll.Wait(fds, 2, App.HasPending ? 0 : -1) < 0)
                break;
            if (App.HasPending)
                fds[0].Ready = Poll.In;
            if (fds[1].Ready != 0) {
                long n = File.ReadBytes(_shellOut, buffer, 4096);
                if (n <= 0)
                    break;   // the shell is gone
                for (long i = 0; i < n; i++)
                    Feed(buffer[i]);
                _lineX = _x;   // whatever we type next starts after the output
                _lineY = _y;
                ShowLine();
            }
            if (fds[0].Ready != 0) {
                Message m = default;
                int r = App.Next(out m, 0);
                if (r < 0)
                    break;
                if (r > 0 && !OnMessage(&m))
                    break;
            }
            Flush();
        }
        App.Destroy(ref *_win);
        if (_shell != 0)
            Process.Kill(_shell);
        return 0;
    }

    static bool StartShell()
    {
        int inRead = -1, inWrite = -1, outRead = -1, outWrite = -1;
        if (File.Pipe(out inRead, out inWrite) < 0 || File.Pipe(out outRead, out outWrite) < 0)
            return false;
        // The child inherits fds 0–2: point them at the pipes for the moment of the spawn.
        long save0 = File.Duplicate(0), save1 = File.Duplicate(1), save2 = File.Duplicate(2);
        File.Redirect(inRead, 0);
        File.Redirect(outWrite, 1);
        File.Redirect(outWrite, 2);
        long pid = Process.Start("/bin/sh");
        File.Redirect((int)save0, 0);
        File.Redirect((int)save1, 1);
        File.Redirect((int)save2, 2);
        File.Close((int)save0);
        File.Close((int)save1);
        File.Close((int)save2);
        File.Close(inRead);
        File.Close(outWrite);
        _shellIn = inWrite;
        _shellOut = outRead;
        if (pid < 0)
            return false;
        _shell = (uint)pid;
        return true;
    }

    // ---------------------------------------------------------------- window protocol

    static bool OnMessage(Message* m)
    {
        App.Track(ref *_win, m);
        switch (m->Type)
        {
            case Protocol.Close:
                return false;
            case Protocol.Focus:
                _focused = m->Value != 0;
                DamageCell(_x, _y);
                break;
            case Protocol.Configure:
                Resize(m->Width, m->Height);
                break;
            case Protocol.Key:
                if (m->Value != 0)
                    OnKey(m->Codepoint, m->Mods);
                break;
            case Protocol.Pointer:
                OnPointer(m);
                break;
        }
        return true;
    }

    // ---------------------------------------------------------------- selection and the clipboard

    static bool _selecting, _selected;
    static int _selStart, _selEnd;   // cell indices (y * cols + x), in either order

    static int CellAt(int px, int py)
    {
        int x = (px - Pad) / _font->Width, y = (py - Pad) / _font->Height;
        x = Clamp(x, 0, _cols - 1);
        y = Clamp(y, 0, _rows - 1);
        return y * _cols + x;
    }

    /// The selection as [a, b], clamped to the grid (false when there is none).
    static bool SelectionRange(out int a, out int b)
    {
        a = _selStart < _selEnd ? _selStart : _selEnd;
        b = _selStart < _selEnd ? _selEnd : _selStart;
        int last = _cols * _rows - 1;
        if (!_selected || a > last)
            return false;
        if (b > last)
            b = last;
        if (a < 0)
            a = 0;
        return true;
    }

    static bool InSelection(int index)
    {
        int a = 0, b = 0;
        return SelectionRange(out a, out b) && index >= a && index <= b;
    }

    static void OnPointer(Message* m)
    {
        if (m->Code == Protocol.PointerButton && m->Codepoint == 0) {   // left: select
            if (m->Value != 0) {
                if (_selected)
                    DamageAll();
                _selecting = true;
                _selected = true;
                _selStart = _selEnd = CellAt(m->X, m->Y);
            } else {
                _selecting = false;
                if (_selStart == _selEnd)
                    _selected = false;   // a click, not a selection
            }
            DamageAll();
        } else if (m->Code == Protocol.PointerButton && m->Codepoint == 2 && m->Value != 0) {   // middle: paste
            Paste();
        } else if (m->Code == Protocol.PointerMove && _selecting) {
            int at = CellAt(m->X, m->Y);
            if (at != _selEnd) {
                _selEnd = at;
                DamageAll();
            }
        }
    }

    /// The selected cells as UTF-8 lines (trailing spaces dropped) on the clipboard.
    static void Copy()
    {
        int a = 0, b = 0;
        if (!SelectionRange(out a, out b))
            return;
        // up to 4 bytes per cell, plus a newline per row the selection crosses
        byte* text = (byte*)Memory.Alloc((ulong)((b - a + 1) * 4 + (b - a) / _cols + 2));
        int n = 0;
        int rowEnd = -1;
        for (int i = a; i <= b; i++) {
            int x = i % _cols;
            if (x == 0 && i != a)
                text[n++] = (byte)'\n';
            if (rowEnd < i) {   // last non-space of this row within the selection
                int y = i / _cols;
                int last = (y + 1) * _cols - 1 < b ? (y + 1) * _cols - 1 : b;
                rowEnd = last;
                while (rowEnd >= i && _cells[rowEnd].Cp == ' ')
                    rowEnd--;
                if (rowEnd < i) {
                    i = last;   // nothing on this row: skip it
                    rowEnd = last;
                    continue;
                }
            }
            if (i <= rowEnd)
                n += Encode(_cells[i].Cp, text + n);
        }
        App.SetClipboard(text, n);
        Memory.Free(text);
    }

    /// The clipboard's text typed into the line being edited (a newline sends the line).
    static void Paste()
    {
        byte* text = (byte*)Memory.Alloc(16384);
        long n = App.GetClipboard(text, 16384);
        byte* s = text;
        uint cp;
        while ((cp = Font.NextCodepoint(ref s)) != 0)
            OnKey(cp == '\r' ? 0u : cp, 0);
        Memory.Free(text);
    }

    static void OnKey(uint cp, uint mods)
    {
        if (cp == 0)
            return;
        bool shift = (mods & Keys.ModShift) != 0;
        if (cp == 3 && shift) {   // Ctrl+Shift+C: copy the selection
            Copy();
            return;
        }
        if (cp == 22 && shift) {  // Ctrl+Shift+V: paste
            Paste();
            return;
        }
        if (cp == 3) {   // Ctrl+C: end what the shell runs (its children)
            InterruptChildren();
            Put("^C\n");
            _lineLength = _lineCursor = 0;
            SendLine();   // an empty line gives a fresh prompt
            return;
        }
        if (cp == 4) {   // Ctrl+D: end of input
            if (_lineLength == 0 && _shellIn >= 0) {
                File.Close(_shellIn);
                _shellIn = -1;
            }
            return;
        }
        if (cp == 12) {   // Ctrl+L
            ClearCells(0, _cols * _rows);
            _x = _y = 0;
            _lineX = _lineY = 0;
            ShowLine();
            DamageAll();
            return;
        }
        if (cp == '\n') {
            HideLineCursor();
            MoveToLineEnd();
            Put("\n");
            Remember();
            SendLine();
            _lineLength = _lineCursor = 0;
            _lineX = _x;
            _lineY = _y;
            return;
        }
        if (cp == '\b') {
            if (_lineCursor > 0) {
                for (int i = _lineCursor; i < _lineLength; i++)
                    _line.Chars[i - 1] = _line.Chars[i];
                _lineLength--;
                _lineCursor--;
                ShowLine();
            }
            return;
        }
        switch (cp)
        {
            case Keys.CharLeft:
                if (_lineCursor > 0)
                    _lineCursor--;
                ShowLine();
                return;
            case Keys.CharRight:
                if (_lineCursor < _lineLength)
                    _lineCursor++;
                ShowLine();
                return;
            case Keys.CharHome:
                _lineCursor = 0;
                ShowLine();
                return;
            case Keys.CharEnd:
                _lineCursor = _lineLength;
                ShowLine();
                return;
            case Keys.CharUp:
                Recall(-1);
                return;
            case Keys.CharDown:
                Recall(1);
                return;
            case Keys.CharDelete:
                if (_lineCursor < _lineLength) {
                    for (int i = _lineCursor + 1; i < _lineLength; i++)
                        _line.Chars[i - 1] = _line.Chars[i];
                    _lineLength--;
                    ShowLine();
                }
                return;
        }
        if (cp == '\t')
            cp = ' ';
        if (cp < 0x20 || cp >= 0x110000 || _lineLength >= MaxLine)
            return;
        for (int i = _lineLength; i > _lineCursor; i--)
            _line.Chars[i] = _line.Chars[i - 1];
        _line.Chars[_lineCursor++] = cp;
        _lineLength++;
        ShowLine();
    }

    static void InterruptChildren()
    {
        ProcessInfo p = default;
        for (uint i = 0; Process.GetInfo(i, out p); i++) {
            if (p.Parent == _shell && p.State == 0)
                Signal.Send(p.Pid, Signal.Int);   // like Ctrl+C on the console: the program may catch it
        }
    }

    static void SendLine()
    {
        if (_shellIn < 0)
            return;
        byte* utf8 = stackalloc byte[MaxLine * 4 + 2];
        int n = 0;
        for (int i = 0; i < _lineLength; i++)
            n += Encode(_line.Chars[i], utf8 + n);
        utf8[n++] = (byte)'\n';
        File.WriteBytes(_shellIn, utf8, (ulong)n);
    }

    static int Encode(uint cp, byte* o)
    {
        if (cp < 0x80) {
            o[0] = (byte)cp;
            return 1;
        }
        if (cp < 0x800) {
            o[0] = (byte)(0xC0 | (cp >> 6));
            o[1] = (byte)(0x80 | (cp & 0x3F));
            return 2;
        }
        if (cp < 0x10000) {
            o[0] = (byte)(0xE0 | (cp >> 12));
            o[1] = (byte)(0x80 | ((cp >> 6) & 0x3F));
            o[2] = (byte)(0x80 | (cp & 0x3F));
            return 3;
        }
        o[0] = (byte)(0xF0 | (cp >> 18));
        o[1] = (byte)(0x80 | ((cp >> 12) & 0x3F));
        o[2] = (byte)(0x80 | ((cp >> 6) & 0x3F));
        o[3] = (byte)(0x80 | (cp & 0x3F));
        return 4;
    }

    // ---------------------------------------------------------------- history (as UTF-8 lines)

    static void Remember()
    {
        if (_lineLength == 0)
            return;
        if (_historyCount == 16) {
            for (int i = 1; i < 16; i++)
                Memory.Copy(&_history->Lines[(i - 1) * 256], &_history->Lines[i * 256], 256);
            _historyCount = 15;
        }
        byte* dst = &_history->Lines[_historyCount * 256];
        int n = 0;
        for (int i = 0; i < _lineLength && n < 250; i++)
            n += Encode(_line.Chars[i], dst + n);
        dst[n] = 0;
        _historyCount++;
        _historyAt = _historyCount;
    }

    static void Recall(int step)
    {
        int at = _historyAt + step;
        if (at < 0 || at > _historyCount)
            return;
        _historyAt = at;
        EraseLine();
        _lineLength = 0;
        if (at < _historyCount) {
            byte* s = &_history->Lines[at * 256];
            uint cp;
            while ((cp = Font.NextCodepoint(ref s)) != 0 && _lineLength < MaxLine)
                _line.Chars[_lineLength++] = cp;
        }
        _lineCursor = _lineLength;
        ShowLine();
    }

    // ---------------------------------------------------------------- the edited line on screen

    static void EraseLine()
    {
        int x = _lineX, y = _lineY;
        for (int i = 0; i <= _lineLength; i++) {
            if (y >= 0 && y < _rows)
                SetCell(x, y, ' ', DefaultFg, DefaultBg);
            if (++x >= _cols) {
                x = 0;
                y++;
            }
        }
    }

    /// Draws the line being typed at (_lineX, _lineY) and puts the cursor in it (scrolling when it wraps off).
    static void ShowLine()
    {
        HideLineCursor();
        // make room: the line may wrap past the bottom
        int end = _lineY * _cols + _lineX + _lineLength;
        while (end / _cols >= _rows && _lineY > 0) {
            Scroll();
            _lineY--;
            end -= _cols;
        }
        int x = _lineX, y = _lineY;
        for (int i = 0; i <= _lineLength; i++) {
            if (i == _lineCursor) {
                _x = x;
                _y = y;
            }
            if (y < _rows)
                SetCell(x, y, i < _lineLength ? _line.Chars[i] : ' ', DefaultFg, DefaultBg);
            if (++x >= _cols) {
                x = 0;
                y++;
            }
        }
        if (_y >= _rows)
            _y = _rows - 1;
        DamageCell(_x, _y);
    }

    static void HideLineCursor() => DamageCell(_x, _y);

    static void MoveToLineEnd()
    {
        int pos = _lineY * _cols + _lineX + _lineLength;
        _x = pos % _cols;
        _y = pos / _cols;
        if (_y >= _rows) {
            _y = _rows - 1;
        }
    }

    // ---------------------------------------------------------------- output from the shell

    static void Put(string s)
    {
        for (byte* p = Str.Bytes(s); *p != 0; p++)
            Feed(*p);
    }

    static void Feed(byte b)
    {
        if (_esc != 0 || b == 0x1B) {
            Escape(b);
            return;
        }
        if (b < 0x80) {
            _utf8Left = 0;
            Char(b);
        } else if ((b & 0xC0) == 0x80) {
            if (_utf8Left == 0)
                return;
            _utf8 = (_utf8 << 6) | (uint)(b & 0x3F);
            if (--_utf8Left == 0)
                Char(_utf8);
        } else if ((b & 0xE0) == 0xC0) {
            _utf8 = (uint)(b & 0x1F);
            _utf8Left = 1;
        } else if ((b & 0xF0) == 0xE0) {
            _utf8 = (uint)(b & 0x0F);
            _utf8Left = 2;
        } else {
            _utf8 = (uint)(b & 0x07);
            _utf8Left = 3;
        }
    }

    static void Char(uint cp)
    {
        switch (cp)
        {
            case '\n':
                DamageCell(_x, _y);
                _x = 0;
                NewLine();
                return;
            case '\r':
                DamageCell(_x, _y);
                _x = 0;
                return;
            case '\b':
                DamageCell(_x, _y);
                if (_x > 0)
                    _x--;
                return;
            case '\t':
                do {
                    Char(' ');
                } while (_x % 4 != 0);
                return;
        }
        if (cp < 0x20)
            return;
        if (_x >= _cols) {
            _x = 0;
            NewLine();
        }
        SetCell(_x, _y, cp, _fg, _bg);
        _x++;
    }

    static void NewLine()
    {
        if (++_y >= _rows) {
            _y = _rows - 1;
            Scroll();
        }
    }

    static void Scroll()
    {
        Memory.Move(_cells, _cells + _cols, (ulong)(_cols * (_rows - 1)) * (ulong)sizeof(Cell));
        ClearCells((_rows - 1) * _cols, _rows * _cols);
        // the pixels move too: cheaper than repainting every cell
        Canvas* c = &_win->Canvas;
        int h = _font->Height;
        for (int y = Pad; y < Pad + (_rows - 1) * h; y++)
            Memory.Copy(c->Row(y), c->Row(y + h), (ulong)c->Width * 4);
        c->FillRect(new Rect(0, Pad + (_rows - 1) * h, c->Width, h), DefaultBg);
        DamageAll();
    }

    static void Escape(byte b)
    {
        if (_esc == 0) {   // ESC
            _esc = 1;
            return;
        }
        if (_esc == 1) {
            if (b == '[') {
                _esc = 2;
                _p0 = _p1 = 0;
                _params = 0;
            } else {
                _esc = 0;
            }
            return;
        }
        if (b >= '0' && b <= '9') {
            if (_params == 0)
                _params = 1;
            if (_params == 1)
                _p0 = _p0 * 10 + (b - '0');
            else if (_params == 2)
                _p1 = _p1 * 10 + (b - '0');
            return;
        }
        if (b == ';') {
            if (_params == 0)
                _params = 1;
            _params++;
            if (_params > 2) {
                // several SGR codes in one sequence: apply what we have and keep going
                Sgr(_p0);
                _p0 = _p1;
                _p1 = 0;
                _params = 2;
            }
            return;
        }
        _esc = 0;
        switch ((char)b)
        {
            case 'm':
                Sgr(_p0);
                if (_params >= 2)
                    Sgr(_p1);
                break;
            case 'J':
                ClearCells(0, _cols * _rows);
                DamageAll();
                break;
            case 'K':
                for (int x = _x; x < _cols; x++)
                    SetCell(x, _y, ' ', _fg, _bg);
                break;
            case 'H':
                DamageCell(_x, _y);
                _y = Clamp((_p0 > 0 ? _p0 : 1) - 1, 0, _rows - 1);
                _x = Clamp((_p1 > 0 ? _p1 : 1) - 1, 0, _cols - 1);
                break;
        }
    }

    static int Clamp(int v, int min, int max) => v < min ? min : v > max ? max : v;

    static void Sgr(int p)
    {
        if (p == 0) {
            _fg = DefaultFg;
            _bg = DefaultBg;
        } else if (p >= 30 && p <= 37) {
            _fg = Palette(p - 30);
        } else if (p >= 90 && p <= 97) {
            _fg = Palette(p - 90 + 8);
        } else if (p >= 40 && p <= 47) {
            _bg = Palette(p - 40);
        } else if (p >= 100 && p <= 107) {
            _bg = Palette(p - 100 + 8);
        } else if (p == 39) {
            _fg = DefaultFg;
        } else if (p == 49) {
            _bg = DefaultBg;
        } else if (p == 1 && _fg == DefaultFg) {
            _fg = 0xFFFFFF;
        }
    }

    static uint Palette(int i) => i switch
    {
        0 => 0x15131Eu, 1 => 0xF2567Au, 2 => 0x7BD88Fu, 3 => 0xFFD866u, 4 => 0x78A9FFu, 5 => 0xD69CFFu, 6 => 0x5AD4E6u,
        7 => 0xD8D6E8u, 8 => 0x5C5873u, 9 => 0xFF7A99u, 10 => 0xA2F0B4u, 11 => 0xFFE59Au, 12 => 0xA6C6FFu,
        13 => 0xE7C2FFu, 14 => 0x9AE9F2u, _ => 0xFFFFFFu,
    };

    // ---------------------------------------------------------------- cells and pixels

    static void ClearCells(int from, int to)
    {
        for (int i = from; i < to; i++) {
            _cells[i].Cp = ' ';
            _cells[i].Fg = DefaultFg;
            _cells[i].Bg = DefaultBg;
        }
    }

    static void SetCell(int x, int y, uint cp, uint fg, uint bg)
    {
        if (x < 0 || y < 0 || x >= _cols || y >= _rows)
            return;
        Cell* c = &_cells[y * _cols + x];
        c->Cp = cp;
        c->Fg = fg;
        c->Bg = bg;
        DamageCell(x, y);
    }

    static Rect CellRect(int x, int y) => new Rect(Pad + x * _font->Width, Pad + y * _font->Height, _font->Width, _font->Height);

    static void DamageCell(int x, int y)
    {
        if (x >= _cols)
            x = _cols - 1;
        if (x < 0 || y < 0 || y >= _rows)
            return;
        _dirty = _dirty.Union(CellRect(x, y));
    }

    static void DamageAll() => _dirty = new Rect(0, 0, _win->Width, _win->Height);

    static void PaintCell(int x, int y)
    {
        Cell* cell = &_cells[y * _cols + x];
        Rect r = CellRect(x, y);
        Canvas* c = &_win->Canvas;
        bool cursor = x == (_x < _cols ? _x : _cols - 1) && y == _y;
        uint bg = cell->Bg, fg = cell->Fg;
        if (InSelection(y * _cols + x)) {
            bg = 0x4A3760;
            fg = 0xFFFFFF;
        }
        if (cursor && _focused) {
            bg = Theme.Accent;
            fg = 0x15131E;
        }
        c->FillRect(r, bg);
        if (cell->Cp != ' ')
            _font->DrawChar(c, r.X, r.Y, cell->Cp, fg);
        if (cursor && !_focused)
            c->DrawRect(r, Theme.Accent);
    }

    /// Repaints the cells under the dirty rectangle and hands it to the desktop.
    static void Flush()
    {
        if (_dirty.IsEmpty)
            return;
        Canvas* c = &_win->Canvas;
        Rect all = new Rect(0, 0, _win->Width, _win->Height);
        if (_dirty.X <= 0 && _dirty.Y <= 0 && _dirty.W >= _win->Width && _dirty.H >= _win->Height)
            c->Clear(DefaultBg);
        int x0 = (_dirty.X - Pad) / _font->Width, x1 = (_dirty.Right - Pad + _font->Width - 1) / _font->Width;
        int y0 = (_dirty.Y - Pad) / _font->Height, y1 = (_dirty.Bottom - Pad + _font->Height - 1) / _font->Height;
        for (int y = Clamp(y0, 0, _rows); y < Clamp(y1, 0, _rows); y++) {
            for (int x = Clamp(x0, 0, _cols); x < Clamp(x1, 0, _cols); x++)
                PaintCell(x, y);
        }
        App.Damage(ref *_win, _dirty.Intersect(all));
        _dirty = new Rect(0, 0, 0, 0);
    }

    static void RedrawAll()
    {
        DamageAll();
        Flush();
    }

    /// The desktop asked for another size: keep the bottom of the text, re-flow nothing.
    static void Resize(int width, int height)
    {
        int cols = (width - 2 * Pad) / _font->Width;
        int rows = (height - 2 * Pad) / _font->Height;
        if (cols < 20)
            cols = 20;
        if (rows < 5)
            rows = 5;
        if (cols > 240)
            cols = 240;
        if (rows > 120)
            rows = 120;
        Cell* cells = Memory.NewArray<Cell>((ulong)(cols * rows));
        for (int i = 0; i < cols * rows; i++) {
            cells[i].Cp = ' ';
            cells[i].Fg = DefaultFg;
            cells[i].Bg = DefaultBg;
        }
        int shift = _y >= rows ? _y - rows + 1 : 0;   // keep the cursor's line visible
        for (int y = 0; y < rows && y + shift < _rows; y++) {
            for (int x = 0; x < cols && x < _cols; x++)
                cells[y * cols + x] = _cells[(y + shift) * _cols + x];
        }
        Memory.Free(_cells);
        _cells = cells;
        _cols = cols;
        _rows = rows;
        _selected = false;   // the selection was in the old grid's cells
        _selecting = false;
        _y -= shift;
        _lineY -= shift;
        if (_lineY < 0)
            _lineY = 0;
        if (_x >= _cols)
            _x = _cols - 1;
        App.Resize(ref *_win, cols * _font->Width + 2 * Pad, rows * _font->Height + 2 * Pad);
        RedrawAll();
    }
}
