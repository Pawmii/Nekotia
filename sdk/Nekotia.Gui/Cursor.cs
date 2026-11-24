namespace Nekotia.Gui;

/// <summary>Software mouse cursors for compositors: drawn last, over everything ('#' outline, '.' fill).</summary>
public static unsafe class Cursor
{
    public const int Width = 12, Height = 19;
    public const int Arrow = 0, Resize = 1, Move = 2;

    const string ArrowShape =
        "#           " +
        "##          " +
        "#.#         " +
        "#..#        " +
        "#...#       " +
        "#....#      " +
        "#.....#     " +
        "#......#    " +
        "#.......#   " +
        "#........#  " +
        "#.........# " +
        "#......#####" +
        "#...#..#    " +
        "#..# #..#   " +
        "#.#  #..#   " +
        "##    #..#  " +
        "#     #..#  " +
        "       ##   " +
        "            ";

    // Diagonal (↘↖) for resizing from the bottom-right corner; hot spot in the middle.
    const string ResizeShape =
        "######      " +
        "#....#      " +
        "#...##      " +
        "#....##     " +
        "#.#...##    " +
        "####...##   " +
        "   ##...####" +
        "    ##...#.#" +
        "     ##....#" +
        "      ##...#" +
        "      #....#" +
        "      ######" +
        "            " +
        "            " +
        "            " +
        "            " +
        "            " +
        "            " +
        "            ";

    // Four arrows for moving a window.
    const string MoveShape =
        "     ##     " +
        "    #..#    " +
        "   #....#   " +
        "  ###..###  " +
        " # #..# #   " +
        "#..#..#..#  " +
        "#........#  " +
        "#..#..#..#  " +
        " # #..# #   " +
        "  ###..###  " +
        "   #....#   " +
        "    #..#    " +
        "     ##     " +
        "            " +
        "            " +
        "            " +
        "            " +
        "            " +
        "            ";

    /// <summary>Where the pointer is inside the shape.</summary>
    public static Point HotSpot(int shape) => shape switch
    {
        Resize => new Point(6, 6),
        Move => new Point(5, 6),
        _ => new Point(0, 0),
    };

    /// <summary>Screen rectangle the cursor covers when the pointer is at (x, y).</summary>
    public static Rect Bounds(int shape, int x, int y)
    {
        Point h = HotSpot(shape);
        return new Rect(x - h.X, y - h.Y, Width + 1, Height);
    }

    public static void Draw(Canvas* c, int shape, int x, int y)
    {
        Point h = HotSpot(shape);
        int ox = x - h.X, oy = y - h.Y;
        for (int row = 0; row < Height; row++) {
            for (int col = 0; col < Width; col++) {
                char ch = shape switch
                {
                    Resize => ResizeShape[row * Width + col],
                    Move => MoveShape[row * Width + col],
                    _ => ArrowShape[row * Width + col],
                };
                if (ch == '#')
                    c->Set(ox + col, oy + row, 0x101018);
                else if (ch == '.')
                    c->Set(ox + col, oy + row, 0xFFFFFF);
            }
        }
    }
}
