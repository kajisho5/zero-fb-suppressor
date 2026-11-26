// ZERO-FB - fast transcendental approximations (SPEC 5.3).
//
// Each function is branch-free on its documented domain and is verified against
// the std:: reference in GoldenParityTest (max absolute error < 1e-5).
//
//   fast_log10    x in [1e-10, 1e3]
//   fast_exp2     x in [-30, 4]
//   fast_tanh     any finite x
//   fast_sigmoid  any finite x
//
// Define ZEROFB_EXACT_MATH to route the zerofb::math:: wrappers to std::. That
// build is the one compared against the FP64 reference at the tight tolerance.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace zerofb {

inline float fast_log10(float x) noexcept
{
    std::uint32_t bits;
    std::memcpy(&bits, &x, sizeof bits);
    int e = int(bits >> 23) - 127;
    bits = (bits & 0x007FFFFFu) | 0x3F800000u;
    float m;
    std::memcpy(&m, &bits, sizeof m); // m in [1, 2)

    // Fold to [sqrt(1/2), sqrt(2)) so the series argument stays small.
    const float fold = m > 1.41421356f ? 1.0f : 0.0f;
    m -= fold * 0.5f * m;
    e += int(fold);

    const float s = (m - 1.0f) / (m + 1.0f); // |s| <= 0.1716
    const float s2 = s * s;
    // ln(m) = 2 atanh(s)
    const float ln = s * (2.0f + s2 * (0.666666667f + s2 * (0.4f + s2 * (0.285714286f + s2 * 0.222222222f))));
    return float(e) * 0.301029996f + ln * 0.434294482f;
}

inline float fast_exp2(float x) noexcept
{
    x = x < -126.0f ? -126.0f : (x > 126.0f ? 126.0f : x);
    const int n = int(x + (x >= 0.0f ? 0.5f : -0.5f)); // round to nearest
    const float t = (x - float(n)) * 0.693147181f;       // |t| <= 0.3466
    const float p = 1.0f + t * (1.0f + t * (0.5f + t * (0.166666667f + t * (0.0416666667f
                         + t * (0.00833333333f + t * (0.00138888889f + t * 0.000198412698f))))));
    const std::uint32_t bits = std::uint32_t(n + 127) << 23;
    float scale;
    std::memcpy(&scale, &bits, sizeof scale);
    return p * scale;
}

inline float fast_tanh(float x) noexcept
{
    // Clamp, evaluate the [7/6] Pade approximant on x/4, then apply the
    // double-angle identity twice: tanh(2a) = 2 tanh(a) / (1 + tanh(a)^2).
    x = x < -9.0f ? -9.0f : (x > 9.0f ? 9.0f : x);
    const float a = 0.25f * x;
    const float a2 = a * a;
    const float num = a * (135135.0f + a2 * (17325.0f + a2 * (378.0f + a2)));
    const float den = 135135.0f + a2 * (62370.0f + a2 * (3150.0f + a2 * 28.0f));
    float t = num / den;
    t = (t + t) / (1.0f + t * t);
    t = (t + t) / (1.0f + t * t);
    return t;
}

inline float fast_sigmoid(float x) noexcept
{
    return 0.5f + 0.5f * fast_tanh(0.5f * x);
}

namespace math {

#if defined(ZEROFB_EXACT_MATH)
inline float log10(float x) noexcept { return std::log10(x); }
inline float exp2(float x) noexcept { return std::exp2(x); }
inline float tanh(float x) noexcept { return std::tanh(x); }
inline float sigmoid(float x) noexcept { return 1.0f / (1.0f + std::exp(-x)); }
#else
inline float log10(float x) noexcept { return fast_log10(x); }
inline float exp2(float x) noexcept { return fast_exp2(x); }
inline float tanh(float x) noexcept { return fast_tanh(x); }
inline float sigmoid(float x) noexcept { return fast_sigmoid(x); }
#endif

} // namespace math
} // namespace zerofb
