// ZERO-FB - band layout and shared constants (SPEC 2.2).
#pragma once

#include <array>
#include <cmath>

namespace zerofb {

constexpr int kNumBands = 32;
constexpr int kNumFeatures = 6;
constexpr int kFeatureSize = kNumFeatures * kNumBands; // 192
constexpr int kMaskSize = 2 * kNumBands;               // 64: fb[32] then rev[32]
constexpr int kMaxChannels = 2;

constexpr double kLowHz = 60.0;
constexpr double kHighHz = 15000.0;
constexpr double kPi = 3.14159265358979323846;
constexpr double kShelfQ = 0.70710678118654752440; // 1 / sqrt(2)

/** Control hop length H for a supported sample rate, 0 when unsupported. */
inline int hopForSampleRate(double sampleRate) noexcept
{
    if (sampleRate == 44100.0) return 15;
    if (sampleRate == 48000.0) return 16;
    if (sampleRate == 96000.0) return 32;
    return 0;
}

inline double alphaS(double tau, double fs) noexcept { return 1.0 - std::exp(-1.0 / (tau * fs)); }
inline double alphaH(double tau, int hop, double fs) noexcept { return 1.0 - std::exp(-double(hop) / (tau * fs)); }

/** Centre frequencies, geometric band edges and sidechain Q of the 32 ERB bands. */
struct BandLayout
{
    std::array<double, kNumBands> fc {}, fLo {}, fHi {}, qSc {}, fEq {};

    BandLayout() noexcept
    {
        auto erbs = [](double f) { return 21.4 * std::log10(0.00437 * f + 1.0); };
        const double lo = erbs(kLowHz), hi = erbs(kHighHz);
        for (int b = 0; b < kNumBands; ++b)
        {
            const double e = lo + double(b) / double(kNumBands - 1) * (hi - lo);
            fc[size_t(b)] = (std::pow(10.0, e / 21.4) - 1.0) / 0.00437;
        }
        fc[0] = kLowHz;
        fc[kNumBands - 1] = kHighHz;

        for (int b = 0; b + 1 < kNumBands; ++b)
        {
            const double edge = std::sqrt(fc[size_t(b)] * fc[size_t(b) + 1]);
            fHi[size_t(b)] = edge;
            fLo[size_t(b) + 1] = edge;
        }
        fLo[0] = fc[0] * fc[0] / fHi[0];
        fHi[kNumBands - 1] = fc[kNumBands - 1] * fc[kNumBands - 1] / fLo[kNumBands - 1];

        for (int b = 0; b < kNumBands; ++b)
        {
            const auto i = size_t(b);
            qSc[i] = fc[i] / (fHi[i] - fLo[i]);
            fEq[i] = fc[i];
        }
        // Shelves: half-gain point on the inner band edge (SPEC 2.2).
        fEq[0] = fHi[0];
        fEq[kNumBands - 1] = fLo[kNumBands - 1];
    }
};

} // namespace zerofb
