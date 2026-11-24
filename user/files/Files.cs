// files — the file manager: a directory listing (folders first) and a preview of the selected file.
//
//   click / ↑↓          select            double click / Enter  open: a folder, a program in /bin (runs it), a text file
//   Backspace / ←      the parent folder  wheel                  scroll
//   Delete (twice)     remove             Ctrl+N                 new folder
//   Ctrl+C             the path → clipboard   Ctrl+V             copy the file whose path is on the clipboard here
//   F5                 read the folder again
using Nekotia;
using Nekotia.Gui;
using Nekotia.User;

namespace FilesApp;

public unsafe struct Item
{
    public fixed byte Name[64];
    public byte Type;        // 1 file, 2 directory, 3 device
    public ulong Size;
}

public static unsafe class Program
{
    const int MaxItems = 512;
    const int Toolbar = 40, Row = 22, Status = 24, ListWidth = 330;

    static Window* _win;
    static Font* _font;
    static byte* _path;          // current folder, absolute
    static Item* _items;
    static int _count, _selected = -1, _scroll;
    static byte* _preview;       // text of the selected file (NUL-terminated), or null
    static long _previewSize;
    static byte* _status;
    static bool _confirmDelete;
    static ulong _lastClick;
    static int _lastClickRow = -1;
    static int _hoverButton = -1;

    public static int Main(int argc, byte** argv)
    {
        if (App.Connect() < 0) {
            Console.WriteLine("files: no desktop is running (start one with `nekowm`)");
            return 1;
        }
        _font = App.SystemFont;
        _path = (byte*)Memory.AllocZeroed(File.MaxPath);
        _items = Memory.NewArray<Item>(MaxItems);
        _preview = (byte*)Memory.AllocZeroed(8192);
        _status = (byte*)Memory.AllocZeroed(160);
        if (argc > 1)
            SetPath(argv[1]);
        else
            SetPath(Str.Bytes("/"));
        _win = Memory.New<Window>();
        if (App.CreateWindow("Files", 780, 500, 0, out *_win) < 0)
            return 1;
        Load();
        Draw();
        while (true) {
            Message m = default;
            if (App.Next(out m, -1) < 0 || m.Type == Protocol.Close)
                break;
            if (m.Type == Protocol.Configure) {
                App.Resize(ref *_win, m.Width < 480 ? 480 : m.Width, m.Height < 240 ? 240 : m.Height);
                Draw();
            } else if (m.Type == Protocol.Key && m.Value != 0) {
                OnKey(m.Code, m.Codepoint, m.Mods);
            } else if (m.Type == Protocol.Pointer) {
                OnPointer(&m);
            }
        }
        App.Destroy(ref *_win);
        return 0;
    }

    // ---------------------------------------------------------------- the folder

    static void SetPath(byte* p)
    {
        int n = 0;
        for (; p[n] != 0 && n < File.MaxPath - 1; n++)
            _path[n] = p[n];
        _path[n] = 0;
    }

    static void Say(string text)
    {
        Text t = new Text(_status, 160);
        t.Write(text);
    }

    /// Path of item `i` into `out`.
    static void ItemPath(int i, byte* output)
    {
        Path.Join(_path, _items[i].Name, output, File.MaxPath);
    }

    static void Load()
    {
        _count = 0;
        _scroll = 0;
        _selected = -1;
        _preview[0] = 0;
        _confirmDelete = false;
        long fd = File.Open(_path, File.Read);
        if (fd < 0) {
            Text t = new Text(_status, 160);
            t.Write("cannot open the folder: ");
            t.Write(Process.ErrorText(fd));
            return;
        }
        DirEntry e = default;
        for (uint i = 0; _count < MaxItems && File.ReadDirectory((int)fd, i, out e) == 1; i++) {
            Item* it = &_items[_count++];
            for (int k = 0; k < 64; k++)
                it->Name[k] = e.Name[k];
            it->Type = e.Type;
            it->Size = e.Size;
        }
        File.Close((int)fd);
        Sort();
        Text s = new Text(_status, 160);
        s.Write($"{_count} items");
    }

    static bool Before(Item* a, Item* b)
    {
        bool ad = a->Type == 2, bd = b->Type == 2;
        if (ad != bd)
            return ad;
        for (int k = 0; k < 64; k++) {
            byte x = a->Name[k], y = b->Name[k];
            if (x >= 'A' && x <= 'Z')
                x = (byte)(x + 32);
            if (y >= 'A' && y <= 'Z')
                y = (byte)(y + 32);
            if (x != y)
                return x < y;
            if (x == 0)
                return false;
        }
        return false;
    }

    static void Sort()
    {
        for (int i = 1; i < _count; i++) {
            Item key = _items[i];
            int j = i - 1;
            while (j >= 0 && Before(&key, &_items[j])) {
                _items[j + 1] = _items[j];
                j--;
            }
            _items[j + 1] = key;
        }
    }

    static void Up()
    {
        int n = (int)Str.Length(_path);
        if (n <= 1)
            return;
        while (n > 1 && _path[n - 1] != '/')
            n--;
        if (n > 1)
            n--;
        _path[n] = 0;
        Load();
        Draw();
    }

    static void Select(int i)
    {
        if (i < 0 || i >= _count)
            return;
        _selected = i;
        _confirmDelete = false;
        int visible = ListRows();
        if (_selected < _scroll)
            _scroll = _selected;
        if (_selected >= _scroll + visible)
            _scroll = _selected - visible + 1;
        LoadPreview();
        Draw();
    }

    static void LoadPreview()
    {
        _preview[0] = 0;
        _previewSize = 0;
        if (_selected < 0 || _items[_selected].Type != 1)
            return;
        byte* p = stackalloc byte[File.MaxPath];
        ItemPath(_selected, p);
        long fd = File.Open(p, File.Read);
        if (fd < 0)
            return;
        long n = File.ReadBytes((int)fd, _preview, 8191);
        File.Close((int)fd);
        if (n < 0)
            n = 0;
        _preview[n] = 0;
        _previewSize = n;
        for (long i = 0; i < n; i++) {
            if (_preview[i] == 0 || (_preview[i] < 9 && _preview[i] != 0)) {   // binary: no text preview
                _preview[0] = 0;
                _previewSize = -1;
                return;
            }
        }
    }

    static bool IsProgram(int i)
    {
        return Str.StartsWith(_path, "/bin") && (_path[4] == 0 || (_path[4] == '/' && _path[5] == 0)) && _items[i].Type == 1;
    }

    static void Open(int i)
    {
        if (i < 0 || i >= _count)
            return;
        byte* p = stackalloc byte[File.MaxPath];
        ItemPath(i, p);
        if (_items[i].Type == 2) {
            SetPath(p);
            Load();
            Draw();
            return;
        }
        if (IsProgram(i)) {
            long pid = Process.Start(p, null);
            Text t = new Text(_status, 160);
            if (pid < 0) {
                t.Write("cannot start: ");
                t.Write(Process.ErrorText(pid));
            } else {
                t.Write($"started {_items[i].Name} (pid {pid})");
            }
            Draw();
            return;
        }
        Select(i);
    }

    static void Delete()
    {
        if (_selected < 0)
            return;
        if (!_confirmDelete) {
            _confirmDelete = true;
            Text t = new Text(_status, 160);
            t.Write("press Delete again to remove ");
            t.Write(_items[_selected].Name);
            Draw();
            return;
        }
        byte* p = stackalloc byte[File.MaxPath];
        ItemPath(_selected, p);
        long r = File.Delete(p);
        int keep = _selected;
        Load();
        Text t2 = new Text(_status, 160);
        if (r < 0) {
            t2.Write("cannot remove: ");
            t2.Write(Process.ErrorText(r));
        } else {
            t2.Write("removed");
        }
        Select(keep < _count ? keep : _count - 1);
        Draw();
    }

    static void NewFolder()
    {
        byte* p = stackalloc byte[File.MaxPath];
        byte* name = stackalloc byte[32];
        for (int n = 1; n < 100; n++) {
            Text t = new Text(name, 32);
            t.Write($"New folder {n}");
            Path.Join(_path, name, p, File.MaxPath);
            if (!File.Exists(p))
                break;
        }
        long r = File.CreateDirectory(p);
        Load();
        if (r < 0) {
            Text t = new Text(_status, 160);
            t.Write("cannot create: ");
            t.Write(Process.ErrorText(r));
        }
        Draw();
    }

    static void CopyPath()
    {
        if (_selected < 0)
            return;
        byte* p = stackalloc byte[File.MaxPath];
        ItemPath(_selected, p);
        App.SetClipboard(p, (long)Str.Length(p));
        Say("path copied");
        Draw();
    }

    /// The clipboard holds a file's path: copy that file into this folder.
    static void Paste()
    {
        byte* from = stackalloc byte[File.MaxPath];
        byte* to = stackalloc byte[File.MaxPath];
        long n = App.GetClipboard(from, File.MaxPath);
        while (n > 0 && (from[n - 1] == '\n' || from[n - 1] == ' '))
            from[--n] = 0;
        if (n == 0 || !File.Exists(from) || File.IsDirectory(from)) {
            Say("the clipboard holds no file path");
            Draw();
            return;
        }
        Path.Join(_path, Path.BaseName(from), to, File.MaxPath);
        if (Str.Equals(from, to)) {
            Say("that file is already here");
            Draw();
            return;
        }
        long r = Tool.CopyFile(from, to);
        Load();
        Text t = new Text(_status, 160);
        if (r < 0) {
            t.Write("cannot copy: ");
            t.Write(Process.ErrorText(r));
        } else {
            t.Write($"copied {r} bytes");
        }
        Draw();
    }

    // ---------------------------------------------------------------- input

    static void OnKey(uint code, uint cp, uint mods)
    {
        if (code == Keys.Down)
            Select(_selected < 0 ? 0 : _selected + 1);
        else if (code == Keys.Up)
            Select(_selected <= 0 ? 0 : _selected - 1);
        else if (code == Keys.Home)
            Select(0);
        else if (code == Keys.End)
            Select(_count - 1);
        else if (cp == '\n')
            Open(_selected);
        else if (cp == '\b' || code == Keys.Left)
            Up();
        else if (code == Keys.Delete)
            Delete();
        else if (code == Keys.F5) {
            Load();
            Draw();
        } else if (cp == 14)    // Ctrl+N
            NewFolder();
        else if (cp == 3)       // Ctrl+C
            CopyPath();
        else if (cp == 22)      // Ctrl+V
            Paste();
    }

    static Rect UpButton() => new Rect(10, 7, 70, 26);

    static Rect FolderButton() => new Rect(88, 7, 110, 26);

    static int ListRows() => (_win->Height - Toolbar - Status) / Row;

    static void OnPointer(Message* m)
    {
        int x = m->X, y = m->Y;
        if (m->Code == Protocol.PointerWheel) {
            _scroll += (int)m->Value * 3;
            int max = _count - ListRows();
            _scroll = _scroll < 0 ? 0 : _scroll > (max < 0 ? 0 : max) ? (max < 0 ? 0 : max) : _scroll;
            Draw();
            return;
        }
        int hover = UpButton().Contains(x, y) ? 0 : FolderButton().Contains(x, y) ? 1 : -1;
        if (m->Code == Protocol.PointerLeave)
            hover = -1;
        if (hover != _hoverButton) {
            _hoverButton = hover;
            Draw();
        }
        if (m->Code != Protocol.PointerButton || m->Value == 0 || m->Codepoint != 0)
            return;
        if (hover == 0) {
            Up();
            return;
        }
        if (hover == 1) {
            NewFolder();
            return;
        }
        if (x < ListWidth && y >= Toolbar && y < _win->Height - Status) {
            int row = _scroll + (y - Toolbar) / Row;
            if (row >= _count)
                return;
            ulong now = Time.UptimeMs();
            if (row == _lastClickRow && now - _lastClick < 400) {
                _lastClickRow = -1;
                Open(row);
                return;
            }
            _lastClick = now;
            _lastClickRow = row;
            Select(row);
        }
    }

    // ---------------------------------------------------------------- drawing

    static void Icon(Canvas* c, int x, int y, byte type)
    {
        if (type == 2) {   // a folder
            c->FillRoundRect(new Rect(x, y + 2, 7, 4), 1, 0xE0B45A);
            c->FillRoundRect(new Rect(x, y + 4, 16, 11), 2, 0xF2C66D);
        } else if (type == 3) {
            c->FillRoundRect(new Rect(x + 1, y + 2, 14, 12), 2, Theme.TextMuted);
        } else {
            c->FillRoundRect(new Rect(x + 2, y, 12, 16), 2, 0xC9C5DA);
            c->FillRect(new Rect(x + 4, y + 5, 8, 1), 0x8E89A6);
            c->FillRect(new Rect(x + 4, y + 8, 8, 1), 0x8E89A6);
            c->FillRect(new Rect(x + 4, y + 11, 6, 1), 0x8E89A6);
        }
    }

    static void Draw()
    {
        Canvas* c = &_win->Canvas;
        int w = _win->Width, h = _win->Height;
        c->Clear(Theme.Window);
        // toolbar: up, new folder, the path
        c->GradientV(new Rect(0, 0, w, Toolbar), 0x2A2540, 0x221E33);
        c->HLine(0, Toolbar - 1, w, Theme.Border);
        Theme.Button(c, _font, UpButton(), "↑ Up", _hoverButton == 0 ? 1 : 0);
        Theme.Button(c, _font, FolderButton(), "New folder", _hoverButton == 1 ? 1 : 0);
        Rect path = new Rect(208, 7, w - 218, 26);
        Theme.Field(c, path, false);
        _font->DrawClipped(c, path.X + 8, path.Y + 5, _path, Theme.Text, path.W - 16);

        // the list
        int rows = ListRows();
        for (int r = 0; r < rows && _scroll + r < _count; r++) {
            int i = _scroll + r;
            Item* it = &_items[i];
            Rect line = new Rect(4, Toolbar + r * Row, ListWidth - 8, Row);
            if (i == _selected)
                c->FillRoundRect(line, 4, 0x3A2F5C);
            Icon(c, line.X + 6, line.Y + 3, it->Type);
            _font->DrawClipped(c, line.X + 28, line.Y + 3, it->Name, Theme.Text, ListWidth - 120);
            if (it->Type == 1) {
                byte* size = stackalloc byte[24];
                Text t = new Text(size, 24);
                t.Write($"{it->Size:size}");
                int sw = _font->Measure(size);
                _font->Draw(c, line.Right - sw - 6, line.Y + 3, size, Theme.TextMuted);
            }
        }
        c->VLine(ListWidth, Toolbar, h - Toolbar - Status, Theme.Border);

        // the preview
        int px = ListWidth + 14, py = Toolbar + 10;
        if (_selected >= 0) {
            Item* it = &_items[_selected];
            _font->DrawClipped(c, px, py, it->Name, Theme.Accent, w - px - 14);
            py += 24;
            byte* info = stackalloc byte[80];
            Text t = new Text(info, 80);
            if (it->Type == 2)
                t.Write("folder — double click or Enter to open");
            else if (IsProgram(_selected))
                t.Write($"program, {it->Size:size} — double click or Enter to run");
            else if (_previewSize < 0)
                t.Write($"binary file, {it->Size:size}");
            else
                t.Write($"text, {it->Size:size}");
            _font->Draw(c, px, py, info, Theme.TextMuted);
            py += 26;
            if (_previewSize > 0)
                DrawText(c, new Rect(px, py, w - px - 12, h - py - Status - 6));
        } else {
            _font->Draw(c, px, py, "Select a file to see what is in it.", Theme.TextMuted);
        }

        // status line
        c->FillRect(new Rect(0, h - Status, w, Status), 0x15131E);
        _font->DrawClipped(c, 10, h - Status + 4, _status, Theme.TextMuted, w - 20);
        App.Present(ref *_win);
    }

    /// The preview text, wrapped at the pane's width, as many lines as fit.
    static void DrawText(Canvas* c, Rect box)
    {
        c->FillRoundRect(box, 6, 0x15131E);
        int cols = (box.W - 16) / _font->Width, lines = (box.H - 12) / _font->Height;
        int x = 0, y = 0;
        byte* s = _preview;
        uint cp;
        while ((cp = Font.NextCodepoint(ref s)) != 0 && y < lines) {
            if (cp == '\n' || x == cols) {
                x = 0;
                y++;
                if (cp == '\n')
                    continue;
                if (y >= lines)
                    break;
            }
            if (cp == '\t')
                cp = ' ';
            if (cp == '\r')
                continue;
            _font->DrawChar(c, box.X + 8 + x * _font->Width, box.Y + 6 + y * _font->Height, cp, Theme.Text);
            x++;
        }
    }
}
