namespace Nekotia.Gui;

/// <summary>
/// A 32-bit pixel surface and the CPU rasteriser that draws on it: rectangles, lines, circles, rounded and
/// anti-aliased shapes, gradients, shadows, blits and (with <see cref="Font"/>) text. Everything is clipped to
/// <see cref="Clip"/>, which never extends past the surface. The pixels belong to someone else — a window's
/// shared memory, a compositor's back buffer, a heap block from <see cref="Create"/>.
/// </summary>
public unsafe struct Canvas
{
    public uint* Pixels;
    public int Width, Height;
    public int Stride;   // pixels per row
    public Rect Clip;

    public Canvas(uint* pixels, int width, int height, int stride)
    {
        Pixels = pixels;
        Width = width;
        Height = height;
        Stride = stride;
        Clip = new Rect(0, 0, width, height);
    }

    /// <summary>A surface with its own zeroed pixels on the heap (<see cref="Free"/> gives them back).</summary>
    public static Canvas Create(int width, int height)
    {
        uint* p = (uint*)Nekotia.User.Memory.AllocZeroed((ulong)width * (ulong)height * 4);
        if (p == null)
            return new Canvas(null, 0, 0, 0);
        return new Canvas(p, width, height, width);
    }

    public void Free()
    {
        Nekotia.User.Memory.Free(Pixels);
        Pixels = null;
        Width = Height = Stride = 0;
        Clip = new Rect(0, 0, 0, 0);
    }

    public Rect Bounds => new Rect(0, 0, Width, Height);

    public void SetClip(Rect r) => Clip = r.Intersect(Bounds);

    public void ResetClip() => Clip = Bounds;

    public uint* Row(int y) => Pixels + (long)y * Stride;

    public uint Get(int x, int y) => Clip.Contains(x, y) ? Pixels[(long)y * Stride + x] : 0;

    public void Set(int x, int y, uint color)
    {
        if (Clip.Contains(x, y))
            Pixels[(long)y * Stride + x] = color;
    }

    public void BlendPixel(int x, int y, uint color, uint alpha)
    {
        if (alpha == 0 || !Clip.Contains(x, y))
            return;
        uint* p = Pixels + (long)y * Stride + x;
        *p = Color.Blend(*p, color, alpha);
    }

    // ---------------------------------------------------------------- rectangles

    public void Clear(uint color) => FillRect(Bounds, color);

    public void FillRect(Rect r, uint color)
    {
        Rect c = r.Intersect(Clip);
        if (c.IsEmpty)
            return;
        uint value = color & 0xFFFFFF;
        for (int y = c.Y; y < c.Bottom; y++)
            Nekotia.User.Memory.Fill32(Row(y) + c.X, value, (ulong)c.W);
    }

    /// <summary>Translucent fill: <paramref name="alpha"/> 0 (nothing) … 255 (opaque).</summary>
    public void FillRectAlpha(Rect r, uint color, uint alpha)
    {
        if (alpha >= 255) {
            FillRect(r, color);
            return;
        }
        Rect c = r.Intersect(Clip);
        if (c.IsEmpty || alpha == 0)
            return;
        for (int y = c.Y; y < c.Bottom; y++) {
            uint* p = Row(y) + c.X;
            for (int x = 0; x < c.W; x++)
                p[x] = Color.Blend(p[x], color, alpha);
        }
    }

    /// <summary>1-pixel outline inside <paramref name="r"/>.</summary>
    public void DrawRect(Rect r, uint color)
    {
        if (r.IsEmpty)
            return;
        FillRect(new Rect(r.X, r.Y, r.W, 1), color);
        FillRect(new Rect(r.X, r.Bottom - 1, r.W, 1), color);
        FillRect(new Rect(r.X, r.Y + 1, 1, r.H - 2), color);
        FillRect(new Rect(r.Right - 1, r.Y + 1, 1, r.H - 2), color);
    }

    public void HLine(int x, int y, int w, uint color) => FillRect(new Rect(x, y, w, 1), color);

    public void VLine(int x, int y, int h, uint color) => FillRect(new Rect(x, y, 1, h), color);

    /// <summary>Vertical gradient from <paramref name="top"/> to <paramref name="bottom"/>.</summary>
    public void GradientV(Rect r, uint top, uint bottom)
    {
        Rect c = r.Intersect(Clip);
        if (c.IsEmpty)
            return;
        for (int y = c.Y; y < c.Bottom; y++) {
            uint color = Color.Mix(top, bottom, r.H > 1 ? (y - r.Y) * 256 / (r.H - 1) : 0);
            Nekotia.User.Memory.Fill32(Row(y) + c.X, color, (ulong)c.W);
        }
    }

    public void GradientH(Rect r, uint left, uint right)
    {
        Rect c = r.Intersect(Clip);
        if (c.IsEmpty)
            return;
        for (int x = c.X; x < c.Right; x++) {
            uint color = Color.Mix(left, right, r.W > 1 ? (x - r.X) * 256 / (r.W - 1) : 0);
            for (int y = c.Y; y < c.Bottom; y++)
                Pixels[(long)y * Stride + x] = color;
        }
    }

    /// <summary>
    /// Filled rectangle with anti-aliased corners of <paramref name="radius"/>. <paramref name="corners"/>: bit 0
    /// top-left, 1 top-right, 2 bottom-left, 3 bottom-right (15 = all).
    /// </summary>
    public void FillRoundRect(Rect r, int radius, uint color, int corners = 15, uint alpha = 255)
    {
        if (r.IsEmpty)
            return;
        if (radius * 2 > r.W)
            radius = r.W / 2;
        if (radius * 2 > r.H)
            radius = r.H / 2;
        if (radius <= 0) {
            FillRectAlpha(r, color, alpha);
            return;
        }
        // The middle band and the two side bands between the corners.
        FillRectAlpha(new Rect(r.X, r.Y + radius, r.W, r.H - 2 * radius), color, alpha);
        FillRectAlpha(new Rect(r.X + radius, r.Y, r.W - 2 * radius, radius), color, alpha);
        FillRectAlpha(new Rect(r.X + radius, r.Bottom - radius, r.W - 2 * radius, radius), color, alpha);
        Corner(r.X, r.Y, radius, color, alpha, (corners & 1) != 0, 0);
        Corner(r.Right - radius, r.Y, radius, color, alpha, (corners & 2) != 0, 1);
        Corner(r.X, r.Bottom - radius, radius, color, alpha, (corners & 4) != 0, 2);
        Corner(r.Right - radius, r.Bottom - radius, radius, color, alpha, (corners & 8) != 0, 3);
    }

    /// <summary>A radius × radius corner square at (x, y): a quarter disc, or the full square when not rounded.</summary>
    void Corner(int x, int y, int radius, uint color, uint alpha, bool round, int which)
    {
        if (!round) {
            FillRectAlpha(new Rect(x, y, radius, radius), color, alpha);
            return;
        }
        // Centre of the quarter circle, in pixel-centre coordinates.
        double cx = (which & 1) == 0 ? x + radius : x;
        double cy = (which & 2) == 0 ? y + radius : y;
        for (int py = 0; py < radius; py++) {
            for (int px = 0; px < radius; px++) {
                double dx = x + px + 0.5 - cx;
                double dy = y + py + 0.5 - cy;
                double d = Maths.Sqrt(dx * dx + dy * dy);
                double cover = radius - d + 0.5;
                if (cover <= 0)
                    continue;
                uint a = cover >= 1 ? alpha : (uint)(cover * alpha);
                BlendPixel(x + px, y + py, color, a);
            }
        }
    }

    /// <summary>1-pixel rounded outline.</summary>
    public void DrawRoundRect(Rect r, int radius, uint color)
    {
        if (r.IsEmpty)
            return;
        if (radius * 2 > r.W)
            radius = r.W / 2;
        if (radius * 2 > r.H)
            radius = r.H / 2;
        FillRect(new Rect(r.X + radius, r.Y, r.W - 2 * radius, 1), color);
        FillRect(new Rect(r.X + radius, r.Bottom - 1, r.W - 2 * radius, 1), color);
        FillRect(new Rect(r.X, r.Y + radius, 1, r.H - 2 * radius), color);
        FillRect(new Rect(r.Right - 1, r.Y + radius, 1, r.H - 2 * radius), color);
        if (radius <= 0)
            return;
        RingQuarter(r.X + radius, r.Y + radius, radius, color, -1, -1);
        RingQuarter(r.Right - radius, r.Y + radius, radius, color, 1, -1);
        RingQuarter(r.X + radius, r.Bottom - radius, radius, color, -1, 1);
        RingQuarter(r.Right - radius, r.Bottom - radius, radius, color, 1, 1);
    }

    void RingQuarter(int cx, int cy, int radius, uint color, int sx, int sy)
    {
        for (int py = 0; py < radius; py++) {
            for (int px = 0; px < radius; px++) {
                double dx = px + 0.5;
                double dy = py + 0.5;
                double d = Maths.Sqrt(dx * dx + dy * dy);
                double cover = 1 - Maths.Abs(d - (radius - 0.5));
                if (cover <= 0)
                    continue;
                int x = sx < 0 ? cx - 1 - px : cx + px;
                int y = sy < 0 ? cy - 1 - py : cy + py;
                BlendPixel(x, y, color, (uint)(cover * 255));
            }
        }
    }

    /// <summary>
    /// Soft drop shadow around <paramref name="r"/>, fading out over <paramref name="size"/> pixels, darkest
    /// (<paramref name="strength"/>, 0…255) at the edge. Drawn outside the rectangle only, offset down by
    /// <paramref name="dy"/>.
    /// </summary>
    public void Shadow(Rect r, int size, uint strength, int dy = 0)
    {
        Rect s = r.Offset(0, dy);
        Rect area = s.Inflate(size).Intersect(Clip);
        if (area.IsEmpty || size <= 0)
            return;
        for (int y = area.Y; y < area.Bottom; y++) {
            int ddy = y < s.Y ? s.Y - y : y >= s.Bottom ? y - s.Bottom + 1 : 0;
            uint* row = Row(y);
            for (int x = area.X; x < area.Right; x++) {
                int ddx = x < s.X ? s.X - x : x >= s.Right ? x - s.Right + 1 : 0;
                if (ddx == 0 && ddy == 0) {
                    if (r.Contains(x, y)) {   // under the window itself: it covers this anyway
                        x = r.Right - 1;
                        continue;
                    }
                    row[x] = Color.Blend(row[x], 0, strength);
                    continue;
                }
                int d2 = ddx * ddx + ddy * ddy;
                if (d2 >= size * size)
                    continue;
                // Quadratic fall-off: (1 - d/size)^2, computed without a square root.
                double t = 1 - Maths.Sqrt(d2) / size;
                uint a = (uint)(strength * t * t);
                if (a != 0)
                    row[x] = Color.Blend(row[x], 0, a);
            }
        }
    }

    // ---------------------------------------------------------------- lines and circles

    /// <summary>1-pixel line (Bresenham).</summary>
    public void Line(int x0, int y0, int x1, int y1, uint color)
    {
        int dx = x1 > x0 ? x1 - x0 : x0 - x1;
        int dy = y1 > y0 ? y0 - y1 : y1 - y0;
        int sx = x0 < x1 ? 1 : -1;
        int sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        while (true) {
            Set(x0, y0, color);
            if (x0 == x1 && y0 == y1)
                break;
            int e2 = 2 * err;
            if (e2 >= dy) {
                err += dy;
                x0 += sx;
            }
            if (e2 <= dx) {
                err += dx;
                y0 += sy;
            }
        }
    }

    /// <summary>Anti-aliased line of <paramref name="width"/> pixels with round ends (coordinates in pixels, fractional ok).</summary>
    public void LineAA(double x0, double y0, double x1, double y1, double width, uint color, uint alpha = 255)
    {
        double half = width / 2;
        int minX = (int)Maths.Floor((x0 < x1 ? x0 : x1) - half - 1);
        int maxX = (int)Maths.Ceiling((x0 > x1 ? x0 : x1) + half + 1);
        int minY = (int)Maths.Floor((y0 < y1 ? y0 : y1) - half - 1);
        int maxY = (int)Maths.Ceiling((y0 > y1 ? y0 : y1) + half + 1);
        Rect box = new Rect(minX, minY, maxX - minX, maxY - minY).Intersect(Clip);
        double vx = x1 - x0, vy = y1 - y0;
        double len2 = vx * vx + vy * vy;
        for (int y = box.Y; y < box.Bottom; y++) {
            for (int x = box.X; x < box.Right; x++) {
                double px = x + 0.5 - x0, py = y + 0.5 - y0;
                double t = len2 > 0 ? (px * vx + py * vy) / len2 : 0;
                if (t < 0)
                    t = 0;
                else if (t > 1)
                    t = 1;
                double ex = px - t * vx, ey = py - t * vy;
                double cover = half + 0.5 - Maths.Sqrt(ex * ex + ey * ey);
                if (cover <= 0)
                    continue;
                BlendPixel(x, y, color, cover >= 1 ? alpha : (uint)(cover * alpha));
            }
        }
    }

    /// <summary>Anti-aliased filled disc.</summary>
    public void FillCircle(double cx, double cy, double radius, uint color, uint alpha = 255)
    {
        Rect box = new Rect((int)(cx - radius - 1), (int)(cy - radius - 1), (int)(radius * 2 + 3), (int)(radius * 2 + 3)).Intersect(Clip);
        for (int y = box.Y; y < box.Bottom; y++) {
            for (int x = box.X; x < box.Right; x++) {
                double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
                double cover = radius + 0.5 - Maths.Sqrt(dx * dx + dy * dy);
                if (cover <= 0)
                    continue;
                BlendPixel(x, y, color, cover >= 1 ? alpha : (uint)(cover * alpha));
            }
        }
    }

    /// <summary>Anti-aliased ring of <paramref name="width"/> pixels centred on <paramref name="radius"/>.</summary>
    public void DrawCircle(double cx, double cy, double radius, double width, uint color)
    {
        double outer = radius + width / 2;
        Rect box = new Rect((int)(cx - outer - 1), (int)(cy - outer - 1), (int)(outer * 2 + 3), (int)(outer * 2 + 3)).Intersect(Clip);
        for (int y = box.Y; y < box.Bottom; y++) {
            for (int x = box.X; x < box.Right; x++) {
                double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
                double d = Maths.Sqrt(dx * dx + dy * dy);
                double cover = width / 2 + 0.5 - Maths.Abs(d - radius);
                if (cover <= 0)
                    continue;
                BlendPixel(x, y, color, cover >= 1 ? 255 : (uint)(cover * 255));
            }
        }
    }

    /// <summary>Filled triangle (no anti-aliasing): every pixel whose centre is inside.</summary>
    public void FillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint color, uint alpha = 255)
    {
        int minX = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
        int maxX = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
        int minY = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
        int maxY = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
        Rect box = new Rect(minX, minY, maxX - minX + 1, maxY - minY + 1).Intersect(Clip);
        long area = (long)(x1 - x0) * (y2 - y0) - (long)(y1 - y0) * (x2 - x0);
        if (area == 0 || box.IsEmpty)
            return;
        for (int y = box.Y; y < box.Bottom; y++) {
            for (int x = box.X; x < box.Right; x++) {
                // edge functions at the pixel centre, doubled to stay in integers
                long px = 2 * x + 1, py = 2 * y + 1;
                long e0 = (2L * x1 - 2L * x0) * (py - 2L * y0) - (2L * y1 - 2L * y0) * (px - 2L * x0);
                long e1 = (2L * x2 - 2L * x1) * (py - 2L * y1) - (2L * y2 - 2L * y1) * (px - 2L * x1);
                long e2 = (2L * x0 - 2L * x2) * (py - 2L * y2) - (2L * y0 - 2L * y2) * (px - 2L * x2);
                bool inside = area > 0 ? e0 >= 0 && e1 >= 0 && e2 >= 0 : e0 <= 0 && e1 <= 0 && e2 <= 0;
                if (inside)
                    BlendPixel(x, y, color, alpha);
            }
        }
    }

    // ---------------------------------------------------------------- blits

    /// <summary>Copies <paramref name="from"/> of <paramref name="src"/> to (x, y) — opaque.</summary>
    public void Blit(Canvas* src, Rect from, int x, int y)
    {
        Rect f = from.Intersect(src->Bounds);
        int dx = x + (f.X - from.X), dy = y + (f.Y - from.Y);
        Rect to = new Rect(dx, dy, f.W, f.H).Intersect(Clip);
        if (to.IsEmpty)
            return;
        int sx = f.X + (to.X - dx), sy = f.Y + (to.Y - dy);
        for (int row = 0; row < to.H; row++) {
            uint* d = Row(to.Y + row) + to.X;
            uint* s = src->Row(sy + row) + sx;
            Nekotia.User.Memory.Copy(d, s, (ulong)to.W * 4);
        }
    }

    /// <summary>Like Blit, but each source pixel's alpha byte says how much of it covers the destination.</summary>
    public void BlitAlpha(Canvas* src, Rect from, int x, int y)
    {
        Rect f = from.Intersect(src->Bounds);
        int dx = x + (f.X - from.X), dy = y + (f.Y - from.Y);
        Rect to = new Rect(dx, dy, f.W, f.H).Intersect(Clip);
        if (to.IsEmpty)
            return;
        int sx = f.X + (to.X - dx), sy = f.Y + (to.Y - dy);
        for (int row = 0; row < to.H; row++) {
            uint* d = Row(to.Y + row) + to.X;
            uint* s = src->Row(sy + row) + sx;
            for (int i = 0; i < to.W; i++) {
                uint a = s[i] >> 24;
                if (a != 0)
                    d[i] = Color.Blend(d[i], s[i], a);
            }
        }
    }

    /// <summary>
    /// A 1-bit-per-pixel shape (rows of <paramref name="bytesPerRow"/> bytes, most significant bit first) in
    /// <paramref name="color"/>, every bit drawn as a <paramref name="scale"/> × <paramref name="scale"/> block.
    /// Glyphs, cursors and icons use it.
    /// </summary>
    public void DrawBits(byte* bits, int width, int height, int bytesPerRow, int x, int y, uint color, int scale = 1)
    {
        Rect box = new Rect(x, y, width * scale, height * scale).Intersect(Clip);
        if (box.IsEmpty)
            return;
        uint value = color & 0xFFFFFF;
        for (int py = box.Y; py < box.Bottom; py++) {
            byte* line = bits + (py - y) / scale * bytesPerRow;
            uint* dst = Row(py);
            for (int px = box.X; px < box.Right; px++) {
                int bx = (px - x) / scale;
                if ((line[bx >> 3] & (0x80 >> (bx & 7))) != 0)
                    dst[px] = value;
            }
        }
    }
}
