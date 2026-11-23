// Small, dependency-free implementations of the libm functions the kernel needs.
// Accuracy is ~1e-15 relative for the common ranges — fine for graphics and statistics.
#include <neko/math.hpp>

namespace neko::math {
namespace {

constexpr double kTwoPi = 2 * kPi;
constexpr double kHalfPi = kPi / 2;
constexpr double kLn2 = 0.69314718055994530942;
constexpr double kTwo52 = 4503599627370496.0;  // 2^52: above this every double is an integer

}  // namespace

double floor(double x) {
    if (!(abs(x) < kTwo52)) return x;  // also NaN/inf
    double t = double(int64_t(x));
    return t > x ? t - 1 : t;
}

double ceil(double x) {
    if (!(abs(x) < kTwo52)) return x;
    double t = double(int64_t(x));
    return t < x ? t + 1 : t;
}

double round(double x) { return x < 0 ? -floor(-x + 0.5) : floor(x + 0.5); }

double fmod(double x, double y) {
    if (y == 0) return __builtin_nan("");
    return x - double(int64_t(x / y)) * y;
}

// sin on [-pi/4, pi/4] / cos on the same range: Taylor series to x^15, error < 1e-16.
static double sin_kernel(double x) {
    double x2 = x * x;
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110 * (1 - x2 / 156 * (1 - x2 / 210)))))));
}

static double cos_kernel(double x) {
    double x2 = x * x;
    return 1 - x2 / 2 * (1 - x2 / 12 * (1 - x2 / 30 * (1 - x2 / 56 * (1 - x2 / 90 * (1 - x2 / 132 * (1 - x2 / 182))))));
}

double sin(double x) {
    if (x != x || abs(x) > 1e15) return __builtin_nan("");
    double k = round(x / kHalfPi);
    double r = x - k * kHalfPi;
    switch (int64_t(k) & 3) {
        case 0: return sin_kernel(r);
        case 1: return cos_kernel(r);
        case 2: return -sin_kernel(r);
        default: return -cos_kernel(r);
    }
}

double cos(double x) { return sin(x + kHalfPi); }
double tan(double x) { return sin(x) / cos(x); }

double atan2(double y, double x) {
    if (x == 0 && y == 0) return 0;
    double ax = abs(x), ay = abs(y);
    double a = (ax > ay ? ay / ax : ax / ay);
    // atan on [0,1]: range-reduce around 1/2 via atan(a) = atan(c) + atan((a-c)/(1+ac)), then series.
    double t = (a - 0.5) / (1 + 0.5 * a);
    double t2 = t * t, s = 0, term = t;
    for (int n = 1; n < 40; n += 2, term *= -t2) s += term / n;
    double r = 0.46364760900080611621 + s;  // atan(1/2)
    if (ay > ax) r = kHalfPi - r;
    if (x < 0) r = kPi - r;
    return y < 0 ? -r : r;
}

double exp(double x) {
    if (x > 709) return __builtin_inf();
    if (x < -745) return 0;
    double k = round(x / kLn2);
    double r = x - k * kLn2, term = 1, s = 1;
    for (int n = 1; n < 25; ++n) s += (term *= r / n);
    // scale by 2^k through the exponent bits
    int64_t e = int64_t(k) + 1023;
    if (e <= 0) return s * 0;  // underflow
    union { uint64_t u; double d; } scale = {uint64_t(e) << 52};
    return s * scale.d;
}

double log(double x) {
    if (x < 0 || x != x) return __builtin_nan("");
    if (x == 0) return -__builtin_inf();
    union { double d; uint64_t u; } v = {x};
    int64_t e = int64_t((v.u >> 52) & 0x7FF) - 1023;
    v.u = (v.u & 0x000FFFFFFFFFFFFFull) | (1023ull << 52);  // mantissa in [1, 2)
    double m = v.d;
    // log(m) = 2 atanh((m-1)/(m+1))
    double z = (m - 1) / (m + 1), z2 = z * z, s = 0, term = z;
    for (int n = 1; n < 41; n += 2, term *= z2) s += term / n;
    return 2 * s + double(e) * kLn2;
}

double pow(double x, double y) {
    if (y == 0) return 1;
    if (x == 0) return 0;
    double iy = floor(y);
    if (iy == y && abs(y) < 64) {  // exact for small integer powers
        double r = 1, b = x;
        int64_t n = int64_t(abs(y));
        while (n) {
            if (n & 1) r *= b;
            b *= b;
            n >>= 1;
        }
        return y < 0 ? 1 / r : r;
    }
    if (x < 0) return __builtin_nan("");
    return exp(y * log(x));
}

}  // namespace neko::math
