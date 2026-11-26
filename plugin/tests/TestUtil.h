// Minimal test harness shared by the ZERO-FB test executables.
#pragma once

#include "../Source/dsp/ZeroFbEngine.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace zerofbtest {

inline int& failures() { static int n = 0; return n; }
inline int& checks() { static int n = 0; return n; }

inline void report(bool ok, const char* expr, const char* file, int line, const std::string& detail = {})
{
    ++checks();
    if (ok)
        return;
    ++failures();
    std::printf("  FAIL %s:%d  %s  %s\n", file, line, expr, detail.c_str());
}

#define ZFB_CHECK(cond) ::zerofbtest::report((cond), #cond, __FILE__, __LINE__)
#define ZFB_CHECK_MSG(cond, msg) ::zerofbtest::report((cond), #cond, __FILE__, __LINE__, (msg))

inline void section(const char* name) { std::printf("[%s]\n", name); std::fflush(stdout); }

inline int finish(const char* suite)
{
    std::printf("%s: %d checks, %d failures\n", suite, checks(), failures());
    return failures() == 0 ? 0 : 1;
}

inline std::string fmt(const char* format, double a = 0, double b = 0, double c = 0, double d = 0)
{
    char buf[256];
    std::snprintf(buf, sizeof buf, format, a, b, c, d);
    return buf;
}

/** Deterministic noise in [-1, 1) (xorshift32), independent of the C library. */
struct Noise
{
    std::uint32_t s = 0x2545F491u;
    float next() noexcept
    {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return float(s >> 8) * (1.0f / 8388608.0f) - 1.0f;
    }
};

/** Estimator that always returns the same masks. */
struct FixedMaskEstimator : zerofb::MaskEstimator
{
    float fb[zerofb::kNumBands], rev[zerofb::kNumBands];
    int resets = 0;

    FixedMaskEstimator() { setAll(1.0f, 1.0f); }
    void setAll(float f, float r)
    {
        for (int b = 0; b < zerofb::kNumBands; ++b) { fb[b] = f; rev[b] = r; }
    }
    void reset() noexcept override { ++resets; }
    void infer(const float*, float* masks) noexcept override
    {
        for (int b = 0; b < zerofb::kNumBands; ++b)
        {
            masks[b] = fb[b];
            masks[zerofb::kNumBands + b] = rev[b];
        }
    }
};

/** Estimator with slowly moving masks, a function of the frame index only. */
struct PatternEstimator : zerofb::MaskEstimator
{
    std::uint64_t frame = 0;
    void reset() noexcept override {}
    void infer(const float*, float* masks) noexcept override
    {
        for (int b = 0; b < zerofb::kNumBands; ++b)
        {
            const double phase = double(frame) * 0.004 + double(b) * 0.7;
            masks[b] = float(0.55 + 0.44 * std::sin(phase));
            masks[zerofb::kNumBands + b] = float(0.6 + 0.39 * std::cos(phase * 0.37));
        }
        ++frame;
    }
};

inline double dbFromLinear(double x) { return 20.0 * std::log10(x); }

/** Amplitude of the component at frequency f (projection on sin and cos). */
inline double toneAmplitude(const std::vector<float>& x, size_t begin, size_t end, double f, double fs)
{
    double re = 0.0, im = 0.0;
    for (size_t n = begin; n < end; ++n)
    {
        const double ph = 2.0 * zerofb::kPi * f * double(n) / fs;
        re += double(x[n]) * std::cos(ph);
        im += double(x[n]) * std::sin(ph);
    }
    return 2.0 * std::sqrt(re * re + im * im) / double(end - begin);
}

} // namespace zerofbtest
