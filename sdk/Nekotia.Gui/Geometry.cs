namespace Nekotia.Gui;

/// <summary>A point in pixels.</summary>
public struct Point
{
    public int X, Y;

    public Point(int x, int y)
    {
        X = x;
        Y = y;
    }
}

/// <summary>A rectangle in pixels: [X, X + W) × [Y, Y + H). Empty when W or H is not positive.</summary>
public struct Rect
{
    public int X, Y, W, H;

    public Rect(int x, int y, int w, int h)
    {
        X = x;
        Y = y;
        W = w;
        H = h;
    }

    public int Right => X + W;
    public int Bottom => Y + H;
    public bool IsEmpty => W <= 0 || H <= 0;

    public bool Contains(int x, int y) => x >= X && y >= Y && x < X + W && y < Y + H;

    public Rect Intersect(Rect o)
    {
        int x0 = X > o.X ? X : o.X;
        int y0 = Y > o.Y ? Y : o.Y;
        int x1 = Right < o.Right ? Right : o.Right;
        int y1 = Bottom < o.Bottom ? Bottom : o.Bottom;
        if (x1 <= x0 || y1 <= y0)
            return new Rect(0, 0, 0, 0);
        return new Rect(x0, y0, x1 - x0, y1 - y0);
    }

    /// <summary>The smallest rectangle around both (an empty one does not count).</summary>
    public Rect Union(Rect o)
    {
        if (IsEmpty)
            return o;
        if (o.IsEmpty)
            return this;
        int x0 = X < o.X ? X : o.X;
        int y0 = Y < o.Y ? Y : o.Y;
        int x1 = Right > o.Right ? Right : o.Right;
        int y1 = Bottom > o.Bottom ? Bottom : o.Bottom;
        return new Rect(x0, y0, x1 - x0, y1 - y0);
    }

    public bool Overlaps(Rect o) => !Intersect(o).IsEmpty;

    public Rect Offset(int dx, int dy) => new Rect(X + dx, Y + dy, W, H);

    /// <summary>Grown by <paramref name="d"/> on every side (shrunk when negative).</summary>
    public Rect Inflate(int d) => new Rect(X - d, Y - d, W + 2 * d, H + 2 * d);

    public long Area => IsEmpty ? 0 : (long)W * H;
}

/// <summary>
/// A set of changed rectangles (what must be redrawn). Overlapping and nearby rectangles merge; when the
/// list is full everything collapses into the bounding box — always correct, sometimes more work.
/// </summary>
public unsafe struct Damage
{
    public const int Max = 32;
    public fixed int Data[Max * 4];
    public int Count;

    public bool IsEmpty => Count == 0;

    public Rect Get(int i) => new Rect(Data[i * 4], Data[i * 4 + 1], Data[i * 4 + 2], Data[i * 4 + 3]);

    void Set(int i, Rect r)
    {
        Data[i * 4] = r.X;
        Data[i * 4 + 1] = r.Y;
        Data[i * 4 + 2] = r.W;
        Data[i * 4 + 3] = r.H;
    }

    public void Clear() => Count = 0;

    public void Add(Rect r)
    {
        if (r.IsEmpty)
            return;
        // Absorb into an existing rectangle when the union does not waste much (≤ 25% more area).
        for (int i = 0; i < Count; i++) {
            Rect e = Get(i);
            Rect u = e.Union(r);
            if (e.Overlaps(r) || u.Area * 4 <= (e.Area + r.Area) * 5) {
                RemoveAt(i);
                Add(u);   // the bigger one may now swallow others
                return;
            }
        }
        if (Count == Max) {
            Rect all = r;
            for (int i = 0; i < Count; i++)
                all = all.Union(Get(i));
            Count = 1;
            Set(0, all);
            return;
        }
        Set(Count++, r);
    }

    void RemoveAt(int i)
    {
        Count--;
        if (i != Count)
            Set(i, Get(Count));
    }

    public Rect Bounds()
    {
        Rect all = new Rect(0, 0, 0, 0);
        for (int i = 0; i < Count; i++)
            all = all.Union(Get(i));
        return all;
    }
}
