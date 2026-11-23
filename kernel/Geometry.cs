namespace Nekotia.OS;

public struct Point
{
    public int X;
    public int Y;

    public Point(int x, int y)
    {
        X = x;
        Y = y;
    }

    public static Point operator +(Point a, Point b) => new Point(a.X + b.X, a.Y + b.Y);

    public int ManhattanLength => Abs(X) + Abs(Y);

    static int Abs(int v) => v < 0 ? -v : v;
}

public struct Rect
{
    public Point Origin;
    public int Width;
    public int Height;

    public int Area => Width * Height;

    public bool Contains(Point p) =>
        p.X >= Origin.X && p.X < Origin.X + Width &&
        p.Y >= Origin.Y && p.Y < Origin.Y + Height;
}

public static class Colors
{
    public const uint White = 0xFFFFFF;
    public const uint Black = 0x101018;
    public const uint Pink = 0xFF8FB8;
    public const uint Peach = 0xFFD7B5;
}
