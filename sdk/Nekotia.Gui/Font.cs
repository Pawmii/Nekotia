using Nekotia.User;

namespace Nekotia.Gui;

/// <summary>
/// A bitmap font in PC Screen Font format (PSF1 or PSF2 with a Unicode table — the console's Terminus 8×16
/// lives in /usr/share/fonts). Text is UTF-8; characters the font lacks are drawn as '?'. Glyphs can be drawn
/// scaled up by whole multiples (headings).
/// </summary>
public unsafe struct Font
{
    public const int DirectMap = 0x2600;   // Latin, Cyrillic, Greek, punctuation, arrows, box drawing, blocks
    public const string DefaultPath = "/usr/share/fonts/ter-v16n.psf";

    public byte* Data;       // the whole file
    public byte* Glyphs;
    public int Count;
    public int Width, Height;
    public int BytesPerRow, BytesPerGlyph;
    public ushort* Map;      // codepoint → glyph + 1 (0 = none), for codepoints below DirectMap
    public int Fallback;

    public bool Loaded => Data != null;

    /// <summary>Reads a .psf file; false when it is missing or not a font.</summary>
    public static bool Load(string path, out Font font) => Load(Str.Bytes(path), out font);

    public static bool Load(byte* path, out Font font)
    {
        font = default;
        long fd = File.Open(path, File.Read);
        if (fd < 0)
            return false;
        FileStat st = default;
        File.Stat((int)fd, out st);
        if (st.Size < 32 || st.Size > (1 << 22)) {
            File.Close((int)fd);
            return false;
        }
        byte* data = (byte*)Memory.Alloc(st.Size);
        long got = data == null ? -1 : File.ReadBytes((int)fd, data, st.Size);
        File.Close((int)fd);
        if (got != (long)st.Size) {
            Memory.Free(data);
            return false;
        }
        font.Data = data;
        font.Map = (ushort*)Memory.AllocZeroed(DirectMap * 2);
        bool ok = false;
        if (data[0] == 0x36 && data[1] == 0x04)
            ok = font.ParsePsf1((long)st.Size);
        else if (Read32(data) == 0x864AB572)
            ok = font.ParsePsf2((long)st.Size);
        if (!ok || font.Width == 0 || font.Height == 0) {
            font.Free();
            return false;
        }
        font.Fallback = font.Map['?'] != 0 ? font.Map['?'] - 1 : 0;
        return true;
    }

    public void Free()
    {
        Memory.Free(Data);
        Memory.Free(Map);
        Data = null;
        Map = null;
    }

    static uint Read32(byte* p) => (uint)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));

    void MapGlyph(uint cp, int glyph)
    {
        if (cp < DirectMap && Map[cp] == 0)
            Map[cp] = (ushort)(glyph + 1);
    }

    bool ParsePsf1(long size)
    {
        byte mode = Data[2];
        Count = (mode & 1) != 0 ? 512 : 256;
        Width = 8;
        Height = Data[3];
        BytesPerRow = 1;
        BytesPerGlyph = Height;
        Glyphs = Data + 4;
        byte* table = Glyphs + Count * BytesPerGlyph;
        byte* end = Data + size;
        if ((mode & 6) == 0) {
            for (int i = 0; i < Count; i++)
                MapGlyph((uint)i, i);
            return true;
        }
        for (int glyph = 0; glyph < Count && table + 2 <= end; glyph++) {
            bool sequence = false;
            for (; table + 2 <= end; table += 2) {
                uint v = (uint)(table[0] | (table[1] << 8));
                if (v == 0xFFFF) {
                    table += 2;
                    break;
                }
                if (v == 0xFFFE)
                    sequence = true;
                else if (!sequence)
                    MapGlyph(v, glyph);
            }
        }
        return true;
    }

    bool ParsePsf2(long size)
    {
        uint headerSize = Read32(Data + 8);
        uint flags = Read32(Data + 12);
        Count = (int)Read32(Data + 16);
        BytesPerGlyph = (int)Read32(Data + 20);
        Height = (int)Read32(Data + 24);
        Width = (int)Read32(Data + 28);
        BytesPerRow = (Width + 7) / 8;
        Glyphs = Data + headerSize;
        byte* table = Glyphs + (long)Count * BytesPerGlyph;
        byte* end = Data + size;
        if (table > end)
            return false;
        if ((flags & 1) == 0) {
            for (int i = 0; i < Count; i++)
                MapGlyph((uint)i, i);
            return true;
        }
        for (int glyph = 0; glyph < Count && table < end; glyph++) {
            bool sequence = false;
            while (table < end && *table != 0xFF) {
                if (*table == 0xFE) {
                    sequence = true;
                    table++;
                    continue;
                }
                uint cp = *table;
                int extra = 0;
                if (cp >= 0xF0) { cp &= 7; extra = 3; }
                else if (cp >= 0xE0) { cp &= 15; extra = 2; }
                else if (cp >= 0xC0) { cp &= 31; extra = 1; }
                table++;
                for (; extra > 0 && table < end; extra--) {
                    cp = (cp << 6) | (uint)(*table & 0x3F);
                    table++;
                }
                if (!sequence)
                    MapGlyph(cp, glyph);
            }
            table++;   // 0xFF
        }
        return true;
    }

    public byte* Glyph(uint cp)
    {
        int g = cp < DirectMap && Map[cp] != 0 ? Map[cp] - 1 : Fallback;
        return Glyphs + (long)g * BytesPerGlyph;
    }

    // ---------------------------------------------------------------- text

    /// <summary>Next codepoint of UTF-8 text at <paramref name="s"/> (advances it); 0 at the end.</summary>
    public static uint NextCodepoint(ref byte* s)
    {
        uint c = *s;
        if (c == 0)
            return 0;
        s++;
        if (c < 0x80)
            return c;
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        c &= extra == 3 ? 7u : extra == 2 ? 15u : 31u;
        for (; extra > 0 && (*s & 0xC0) == 0x80; extra--) {
            c = (c << 6) | (uint)(*s & 0x3F);
            s++;
        }
        return c;
    }

    /// <summary>Characters (codepoints) in UTF-8 text.</summary>
    public static int CountChars(byte* s)
    {
        int n = 0;
        while (NextCodepoint(ref s) != 0)
            n++;
        return n;
    }

    /// <summary>Width in pixels of one line of text.</summary>
    public int Measure(byte* text, int scale = 1)
    {
        int n = 0;
        byte* s = text;
        uint cp;
        while ((cp = NextCodepoint(ref s)) != 0 && cp != '\n')
            n++;
        return n * Width * scale;
    }

    public int Measure(string text, int scale = 1) => Measure(Str.Bytes(text), scale);

    /// <summary>One character; returns its advance.</summary>
    public int DrawChar(Canvas* canvas, int x, int y, uint cp, uint color, int scale = 1)
    {
        canvas->DrawBits(Glyph(cp), Width, Height, BytesPerRow, x, y, color, scale);
        return Width * scale;
    }

    /// <summary>
    /// UTF-8 text with its top-left corner at (x, y); stops at '\n' or after <paramref name="maxWidth"/> pixels
    /// (0 = no limit). Returns the width drawn.
    /// </summary>
    public int Draw(Canvas* canvas, int x, int y, byte* text, uint color, int scale = 1, int maxWidth = 0)
    {
        int cx = x;
        int advance = Width * scale;
        byte* s = text;
        uint cp;
        while ((cp = NextCodepoint(ref s)) != 0 && cp != '\n') {
            if (maxWidth > 0 && cx + advance > x + maxWidth)
                break;
            canvas->DrawBits(Glyph(cp), Width, Height, BytesPerRow, cx, y, color, scale);
            cx += advance;
        }
        return cx - x;
    }

    public int Draw(Canvas* canvas, int x, int y, string text, uint color, int scale = 1, int maxWidth = 0) =>
        Draw(canvas, x, y, Str.Bytes(text), color, scale, maxWidth);

    /// <summary>Text cut to <paramref name="maxWidth"/> with a trailing "…" when it does not fit (no background touched).</summary>
    public int DrawClipped(Canvas* canvas, int x, int y, byte* text, uint color, int maxWidth, int scale = 1)
    {
        int advance = Width * scale;
        int fits = maxWidth / advance;
        int total = CountChars(text);
        if (total <= fits)
            return Draw(canvas, x, y, text, color, scale);
        if (fits <= 0)
            return 0;
        int cx = x;
        byte* s = text;
        for (int i = 0; i < fits - 1; i++) {
            uint cp = NextCodepoint(ref s);
            cx += DrawChar(canvas, cx, y, cp, color, scale);
        }
        cx += DrawChar(canvas, cx, y, 0x2026, color, scale);   // …
        return cx - x;
    }

    /// <summary>Centered in <paramref name="box"/> (one line).</summary>
    public void DrawCentered(Canvas* canvas, Rect box, byte* text, uint color, int scale = 1)
    {
        int w = Measure(text, scale);
        if (w > box.W)
            w = box.W;
        DrawClipped(canvas, box.X + (box.W - w) / 2, box.Y + (box.H - Height * scale) / 2, text, color, box.W, scale);
    }

    public void DrawCentered(Canvas* canvas, Rect box, string text, uint color, int scale = 1) =>
        DrawCentered(canvas, box, Str.Bytes(text), color, scale);
}
