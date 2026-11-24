// paint — draw with the mouse: a palette, three brush sizes, an eraser and "New". The picture lives in its own
// canvas (1600 × 1000), the window shows its top-left part — resizing never loses what was drawn.
using Nekotia;
using Nekotia.Gui;
using Nekotia.User;

namespace PaintApp;

public static unsafe class Program
{
    const int Toolbar = 44;
    const uint Paper = 0xFBF8F3;

    static Window* _win;
    static Font* _font;
    static Canvas _picture;
    static uint _color = 0x2A2438;
    static int _size = 5;     // brush width; -1 = eraser
    static bool _drawing;
    static int _lastX, _lastY;
    static int _hover = -1;

    public static int Main()
    {
        if (App.Connect() < 0) {
            Console.WriteLine("paint: no desktop is running (start one with `nekowm`)");
            return 1;
        }
        _font = App.SystemFont;
        _picture = Canvas.Create(1600, 1000);
        if (_picture.Pixels == null)
            return 1;
        _picture.Clear(Paper);
        _win = Memory.New<Window>();
        if (App.CreateWindow("Paint", 720, 520, 0, out *_win) < 0)
            return 1;
        DrawAll();
        while (true) {
            Message m = default;
            int r = App.Next(out m, -1);
            if (r < 0)
                break;
            if (m.Type == Protocol.Close)
                break;
            if (m.Type == Protocol.Configure) {
                App.Resize(ref *_win, m.Width < 420 ? 420 : m.Width, m.Height < 200 ? 200 : m.Height);
                DrawAll();
            } else if (m.Type == Protocol.Pointer) {
                OnPointer(&m);
            }
        }
        App.Destroy(ref *_win);
        return 0;
    }

    static uint PaletteColor(int i) => i switch
    {
        0 => 0x2A2438u, 1 => 0xF2567Au, 2 => 0xFF9F43u, 3 => 0xFFD866u, 4 => 0x7BD88Fu, 5 => 0x5AD4E6u,
        6 => 0x78A9FFu, 7 => 0xC6A0F6u, 8 => 0xF5A3C7u, 9 => 0x8B5E3Cu, 10 => 0x9C97B5u, _ => 0xFFFFFFu,
    };

    // Toolbar items: 0–11 colours, 12–14 brush sizes, 15 eraser, 16 new.
    static Rect Item(int i)
    {
        if (i < 12)
            return new Rect(10 + i * 26, 11, 22, 22);
        if (i < 15)
            return new Rect(10 + 12 * 26 + 12 + (i - 12) * 30, 8, 28, 28);
        if (i == 15)
            return new Rect(10 + 12 * 26 + 12 + 3 * 30 + 10, 8, 64, 28);
        return new Rect(_win->Width - 74, 8, 64, 28);
    }

    static int ItemAt(int x, int y)
    {
        for (int i = 0; i <= 16; i++) {
            if (Item(i).Contains(x, y))
                return i;
        }
        return -1;
    }

    static int SizeOf(int i) => i == 12 ? 2 : i == 13 ? 5 : 12;

    static void DrawToolbar()
    {
        Canvas* c = &_win->Canvas;
        c->SetClip(new Rect(0, 0, _win->Width, Toolbar));
        c->GradientV(new Rect(0, 0, _win->Width, Toolbar), 0x2A2540, 0x221E33);
        c->HLine(0, Toolbar - 1, _win->Width, Theme.Border);
        for (int i = 0; i < 12; i++) {
            Rect r = Item(i);
            uint col = PaletteColor(i);
            if (col == _color && _size > 0)
                c->FillRoundRect(r.Inflate(3), 7, Theme.Accent);
            c->FillRoundRect(r, 5, col);
        }
        for (int i = 12; i < 15; i++) {
            Rect r = Item(i);
            bool on = _size == SizeOf(i);
            c->FillRoundRect(r, 6, on ? 0x4A3760u : i == _hover ? Theme.SurfaceHover : Theme.Surface);
            c->FillCircle(r.X + r.W / 2.0, r.Y + r.H / 2.0, SizeOf(i) / 2.0 + 0.5, Theme.Text);
        }
        Theme.Button(c, _font, Item(15), "Eraser", _size < 0 ? 3 : _hover == 15 ? 1 : 0);
        Theme.Button(c, _font, Item(16), "New", _hover == 16 ? 1 : 0);
        c->ResetClip();
    }

    static void DrawAll()
    {
        Canvas* c = &_win->Canvas;
        fixed (Canvas* p = &_picture) {
            c->Blit(p, new Rect(0, 0, _win->Width, _win->Height - Toolbar), 0, Toolbar);
        }
        c->FillRect(new Rect(1600, Toolbar, _win->Width - 1600, _win->Height), Theme.Window);
        c->FillRect(new Rect(0, Toolbar + 1000, _win->Width, _win->Height), Theme.Window);
        DrawToolbar();
        App.Present(ref *_win);
    }

    static void Stroke(int x0, int y0, int x1, int y1)
    {
        bool eraser = _size < 0;
        double width = eraser ? 22 : _size;
        uint color = eraser ? Paper : _color;
        _picture.LineAA(x0, y0, x1, y1, width, color);
        // show the changed part
        int pad = (int)width + 2;
        Rect r = new Rect((x0 < x1 ? x0 : x1) - pad, (y0 < y1 ? y0 : y1) - pad,
            (x0 > x1 ? x0 - x1 : x1 - x0) + 2 * pad, (y0 > y1 ? y0 - y1 : y1 - y0) + 2 * pad);
        Canvas* c = &_win->Canvas;
        Rect on = r.Offset(0, Toolbar).Intersect(new Rect(0, Toolbar, _win->Width, _win->Height - Toolbar));
        fixed (Canvas* p = &_picture) {
            c->Blit(p, on.Offset(0, -Toolbar), on.X, on.Y);
        }
        App.Damage(ref *_win, on);
    }

    static void OnPointer(Message* m)
    {
        int x = m->X, y = m->Y;
        if (m->Code == Protocol.PointerButton) {
            if (m->Value != 0 && y < Toolbar) {
                Pick(ItemAt(x, y));
                return;
            }
            _drawing = m->Value != 0 && y >= Toolbar;
            if (_drawing) {
                _lastX = x;
                _lastY = y - Toolbar;
                Stroke(_lastX, _lastY, _lastX, _lastY);
            }
            return;
        }
        if (m->Code == Protocol.PointerMove) {
            if (_drawing) {
                Stroke(_lastX, _lastY, x, y - Toolbar);
                _lastX = x;
                _lastY = y - Toolbar;
                return;
            }
            int hover = y < Toolbar ? ItemAt(x, y) : -1;
            if (hover != _hover) {
                _hover = hover;
                DrawToolbar();
                App.Damage(ref *_win, new Rect(0, 0, _win->Width, Toolbar));
            }
        }
        if (m->Code == Protocol.PointerLeave && _hover >= 0) {
            _hover = -1;
            DrawToolbar();
            App.Damage(ref *_win, new Rect(0, 0, _win->Width, Toolbar));
        }
    }

    static void Pick(int item)
    {
        if (item < 0)
            return;
        if (item < 12) {
            _color = PaletteColor(item);
            if (_size < 0)
                _size = 5;
        } else if (item < 15) {
            _size = SizeOf(item);
        } else if (item == 15) {
            _size = -1;
        } else {
            _picture.Clear(Paper);
            DrawAll();
            return;
        }
        DrawToolbar();
        App.Damage(ref *_win, new Rect(0, 0, _win->Width, Toolbar));
    }
}
