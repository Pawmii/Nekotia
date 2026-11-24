namespace Nekotia.Gui;

/// <summary>
/// The default look ("Neko Night"): colours and a few drawn controls shared by applications, so programs look
/// alike under any desktop. A desktop may draw its own decorations differently.
/// </summary>
public static unsafe class Theme
{
    public const uint Window = 0x1E1B2B;       // window content background
    public const uint Surface = 0x27233A;      // panels, inputs
    public const uint SurfaceHover = 0x332E4A;
    public const uint Border = 0x3D3756;
    public const uint Text = 0xE6E3F2;
    public const uint TextMuted = 0x9C97B5;
    public const uint Accent = 0xF5A3C7;       // neko pink
    public const uint AccentDark = 0xC4789E;
    public const uint Accent2 = 0x8FB8FF;      // blue
    public const uint Good = 0x7BD88F, Warn = 0xFFD866, Bad = 0xF2567A;

    public const int Radius = 6;

    /// <summary>A push button; <paramref name="state"/>: 0 normal, 1 hovered, 2 pressed, 3 default (accent).</summary>
    public static void Button(Canvas* c, Font* f, Rect r, byte* label, int state)
    {
        uint bg = state switch
        {
            1 => SurfaceHover,
            2 => Border,
            3 => Accent,
            _ => Surface,
        };
        c->FillRoundRect(r, Radius, bg);
        if (state != 3)
            c->DrawRoundRect(r, Radius, Border);
        f->DrawCentered(c, r.Offset(0, state == 2 ? 1 : 0), label, state == 3 ? 0x1E1B2B : Text);
    }

    public static void Button(Canvas* c, Font* f, Rect r, string label, int state) => Button(c, f, r, Str.Bytes(label), state);

    /// <summary>A sunken box for text input or lists.</summary>
    public static void Field(Canvas* c, Rect r, bool focused)
    {
        c->FillRoundRect(r, 4, 0x15131E);
        c->DrawRoundRect(r, 4, focused ? Accent : Border);
    }

    /// <summary>A horizontal bar filled to <paramref name="permille"/> / 1000.</summary>
    public static void Bar(Canvas* c, Rect r, int permille, uint color)
    {
        c->FillRoundRect(r, r.H / 2, Surface);
        if (permille < 0)
            permille = 0;
        if (permille > 1000)
            permille = 1000;
        int w = r.W * permille / 1000;
        if (w > 0)
            c->FillRoundRect(new Rect(r.X, r.Y, w < r.H ? r.H : w, r.H), r.H / 2, color);
    }

    /// <summary>Section heading: accent text and a thin line.</summary>
    public static void Heading(Canvas* c, Font* f, int x, int y, int width, byte* text)
    {
        int w = f->Draw(c, x, y, text, Accent);
        c->HLine(x + w + 8, y + f->Height / 2, width - w - 8, Border);
    }
}
