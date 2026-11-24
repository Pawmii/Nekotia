namespace Nekotia.Gui;

/// <summary>
/// Colours are <c>uint</c> 0xAARRGGBB. Surfaces store 0x00RRGGBB (the display format, XRGB8888); the alpha byte
/// matters only where a method says so (images with transparency, BlendPixel).
/// </summary>
public static class Color
{
    public const uint Black = 0x000000, White = 0xFFFFFF, Transparent = 0;

    public static uint Rgb(int r, int g, int b) => ((uint)(r & 255) << 16) | ((uint)(g & 255) << 8) | (uint)(b & 255);

    public static uint Argb(int a, int r, int g, int b) => ((uint)(a & 255) << 24) | Rgb(r, g, b);

    public static int R(uint c) => (int)((c >> 16) & 255);
    public static int G(uint c) => (int)((c >> 8) & 255);
    public static int B(uint c) => (int)(c & 255);
    public static int A(uint c) => (int)(c >> 24);

    /// <summary><paramref name="src"/> over <paramref name="dst"/> with coverage <paramref name="alpha"/> (0…255).</summary>
    public static uint Blend(uint dst, uint src, uint alpha)
    {
        if (alpha >= 255)
            return src & 0xFFFFFF;
        if (alpha == 0)
            return dst;
        uint inv = 255 - alpha;
        uint rb = (((src & 0xFF00FF) * alpha + (dst & 0xFF00FF) * inv) >> 8) & 0xFF00FF;
        uint g = (((src & 0x00FF00) * alpha + (dst & 0x00FF00) * inv) >> 8) & 0x00FF00;
        return rb | g;
    }

    /// <summary>Linear mix: t = 0 → a, t = 256 → b.</summary>
    public static uint Mix(uint a, uint b, int t)
    {
        if (t <= 0)
            return a & 0xFFFFFF;
        if (t >= 256)
            return b & 0xFFFFFF;
        return Blend(a, b, (uint)(t * 255 / 256));
    }

    /// <summary>Brighter (<paramref name="amount"/> &gt; 0) or darker (&lt; 0), −255…255.</summary>
    public static uint Shade(uint c, int amount)
    {
        return amount >= 0 ? Blend(c, White, (uint)amount) : Blend(c, Black, (uint)(-amount));
    }
}
