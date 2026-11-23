// Floating-point helpers (no libm in the kernel). SSE2 is always available on x86_64.
#pragma once

#include <neko/types.hpp>

namespace neko::math {

constexpr double kPi = 3.14159265358979323846;

inline double sqrt(double x) { return __builtin_sqrt(x); }  // sqrtsd
inline double abs(double x) { return __builtin_fabs(x); }
double floor(double x);
double ceil(double x);
double round(double x);
double fmod(double x, double y);
double sin(double x);
double cos(double x);
double tan(double x);
double atan2(double y, double x);
double exp(double x);
double log(double x);
double pow(double x, double y);

}  // namespace neko::math
