// ZERO-FB - Stage 6A serial 32-band EQ and Stage 5B gain-coupling correction.
// SPEC 2.3 and 2.6.
#pragma once

#include "CytomicSvf.h"
#include "FastMath.h"

#include <algorithm>

namespace zerofb {

class SerialPeakingEq
{
public:
    static constexpr double kCouplingRefDb = -12.0;
    static constexpr int kCouplingHalfWidth = 6;
    static constexpr int kPgsIterations = 3;

    void prepare(double sampleRate, int hopLength) noexcept
    {
        hop = hopLength;
        invHop = 1.0f / float(hop);
        const BandLayout layout;
        for (int b = 0; b < kNumBands; ++b)
        {
            gBase[b] = float(std::tan(kPi * layout.fEq[size_t(b)] / sampleRate));
            q[b] = float(isShelf(b) ? kShelfQ : layout.qSc[size_t(b)]);
        }

        // Coupling matrix: dB response of band j at f_b per dB of band j gain,
        // measured from the analytic response at the reference gain. The
        // diagonal is measured too (it is not 1 for the shelves).
        const double A = std::pow(10.0, kCouplingRefDb / 40.0);
        for (int j = 0; j < kNumBands; ++j)
        {
            const auto jj = size_t(j);
            const double gb = std::tan(kPi * layout.fEq[jj] / sampleRate);
            const auto d = j == 0 ? svf::lowShelf<double>(gb, A, std::sqrt(A), kShelfQ)
                         : j == kNumBands - 1 ? svf::highShelf<double>(gb, A, std::sqrt(A), kShelfQ)
                         : svf::peak<double>(gb, A, layout.qSc[jj]);
            for (int b = 0; b < kNumBands; ++b)
            {
                double c = 0.0;
                if (std::abs(b - j) <= kCouplingHalfWidth)
                    c = 20.0 * std::log10(std::abs(svf::response(d, layout.fc[size_t(b)], sampleRate))) / kCouplingRefDb;
                coupling[b][j] = float(c);
            }
        }

        float unity[kNumBands];
        for (int b = 0; b < kNumBands; ++b)
            unity[b] = 0.0f;
        setImmediate(unity);
        clearState();
    }

    void clearState() noexcept
    {
        for (int c = 0; c < kMaxChannels; ++c)
            for (int b = 0; b < kNumBands; ++b)
                state[c][b] = SvfState {};
    }

    /** Stage 5B: Projected Gauss-Seidel, in place, non-positive gains. */
    void solveCoupling(const float* gTarget, float floorDb, float* gEq) const noexcept
    {
        for (int b = 0; b < kNumBands; ++b)
            gEq[b] = gTarget[b];
        for (int it = 0; it < kPgsIterations; ++it)
        {
            for (int b = 0; b < kNumBands; ++b)
            {
                float acc = gTarget[b];
                const int lo = std::max(0, b - kCouplingHalfWidth);
                const int hi = std::min(kNumBands - 1, b + kCouplingHalfWidth);
                for (int j = lo; j <= hi; ++j)
                    if (j != b)
                        acc -= coupling[b][j] * gEq[j];
                gEq[b] = std::min(0.0f, std::max(floorDb, acc / coupling[b][b]));
            }
        }
    }

    /** Sets the gains reached at the end of the next hop (linear coefficient
        interpolation in between). Call once per hop boundary. */
    void setTargetGainsDb(const float* gEq) noexcept
    {
        for (int b = 0; b < kNumBands; ++b)
        {
            const SvfCoeffs next = design(b, gEq[b]);
            // Start from the exact end point of the previous hop.
            a1p[b] += a1d[b];  a2p[b] += a2d[b];  a3p[b] += a3d[b];
            m0p[b] += m0d[b];  m1p[b] += m1d[b];  m2p[b] += m2d[b];
            a1d[b] = next.a1 - a1p[b];  a2d[b] = next.a2 - a2p[b];  a3d[b] = next.a3 - a3p[b];
            m0d[b] = next.m0 - m0p[b];  m1d[b] = next.m1 - m1p[b];  m2d[b] = next.m2 - m2p[b];
            gainsDb[b] = gEq[b];
        }
    }

    /** Jumps to the given gains with no interpolation. */
    void setImmediate(const float* gEq) noexcept
    {
        for (int b = 0; b < kNumBands; ++b)
        {
            const SvfCoeffs c = design(b, gEq[b]);
            a1p[b] = c.a1;  a2p[b] = c.a2;  a3p[b] = c.a3;
            m0p[b] = c.m0;  m1p[b] = c.m1;  m2p[b] = c.m2;
            a1d[b] = a2d[b] = a3d[b] = m0d[b] = m1d[b] = m2d[b] = 0.0f;
            gainsDb[b] = gEq[b];
        }
    }

    /** One sample through all 32 sections. `m` is the position inside the
        current hop, 0 .. H-1; coefficients are c_prev + (m+1)/H (c_next - c_prev). */
    inline void tick(float* samples, int numChannels, int m) noexcept
    {
        const float t = float(m + 1) * invHop;
        for (int b = 0; b < kNumBands; ++b)
        {
            SvfCoeffs c;
            c.a1 = a1p[b] + t * a1d[b];
            c.a2 = a2p[b] + t * a2d[b];
            c.a3 = a3p[b] + t * a3d[b];
            c.m0 = m0p[b] + t * m0d[b];
            c.m1 = m1p[b] + t * m1d[b];
            c.m2 = m2p[b] + t * m2d[b];
            for (int ch = 0; ch < numChannels; ++ch)
                samples[ch] = state[ch][b].tick(c, samples[ch]);
        }
    }

    const float* bandGainsDb() const noexcept { return gainsDb; }
    float couplingAt(int b, int j) const noexcept { return coupling[b][j]; }

private:
    static bool isShelf(int b) noexcept { return b == 0 || b == kNumBands - 1; }

    SvfCoeffs design(int b, float gainDb) const noexcept
    {
        // A = 10^(G/40) = 2^(G * log2(10) / 40)
        const float A = math::exp2(gainDb * 0.083048202f);
        if (b == 0)
            return svf::lowShelf<float>(gBase[b], A, math::exp2(gainDb * 0.041524101f), q[b]).c;
        if (b == kNumBands - 1)
            return svf::highShelf<float>(gBase[b], A, math::exp2(gainDb * 0.041524101f), q[b]).c;
        return svf::peak<float>(gBase[b], A, q[b]).c;
    }

    int hop = 16;
    float invHop = 1.0f / 16.0f;
    float gBase[kNumBands] {}, q[kNumBands] {};
    float coupling[kNumBands][kNumBands] {};
    float gainsDb[kNumBands] {};
    alignas(32) float a1p[kNumBands] {}, a2p[kNumBands] {}, a3p[kNumBands] {};
    alignas(32) float m0p[kNumBands] {}, m1p[kNumBands] {}, m2p[kNumBands] {};
    alignas(32) float a1d[kNumBands] {}, a2d[kNumBands] {}, a3d[kNumBands] {};
    alignas(32) float m0d[kNumBands] {}, m1d[kNumBands] {}, m2d[kNumBands] {};
    SvfState state[kMaxChannels][kNumBands] {};
};

} // namespace zerofb
