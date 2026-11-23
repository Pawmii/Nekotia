namespace Nekotia;

/// <summary>
/// Floating-point math for kernel code (runtime/src/core/math.cpp — there is no libm).
/// Named Maths so it never clashes with System.Math.
/// </summary>
public static class Maths
{
    public const double Pi = 3.14159265358979323846;

    [Native("::neko::math::sqrt")] public static extern double Sqrt(double x);
    [Native("::neko::math::abs")] public static extern double Abs(double x);
    [Native("::neko::math::floor")] public static extern double Floor(double x);
    [Native("::neko::math::ceil")] public static extern double Ceiling(double x);
    [Native("::neko::math::round")] public static extern double Round(double x);
    [Native("::neko::math::sin")] public static extern double Sin(double x);
    [Native("::neko::math::cos")] public static extern double Cos(double x);
    [Native("::neko::math::tan")] public static extern double Tan(double x);
    [Native("::neko::math::atan2")] public static extern double Atan2(double y, double x);
    [Native("::neko::math::exp")] public static extern double Exp(double x);
    [Native("::neko::math::log")] public static extern double Log(double x);
    [Native("::neko::math::pow")] public static extern double Pow(double x, double y);

    public static double Min(double a, double b) => a < b ? a : b;
    public static double Max(double a, double b) => a > b ? a : b;
    public static double Clamp(double v, double lo, double hi) => v < lo ? lo : v > hi ? hi : v;
}
