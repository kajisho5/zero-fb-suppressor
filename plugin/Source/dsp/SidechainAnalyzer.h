// ZERO-FB - control path: sidechain bank and the 6-channel feature tensor.
// SPEC 2.4. training/erb_dsp.py is the reference for every formula here.
#pragma once

#include "CytomicSvf.h"
#include "FastMath.h"

#include <algorithm>

namespace zerofb {

class SidechainAnalyzer
{
public:
    static constexpr int kModBlockFrames = 16;
    static constexpr int kModSlots = 15;
    /** Sidechain input is clamped to this magnitude (+24 dBFS) so that the
        band powers stay finite for any finite input. */
    static constexpr float kInputLimit = 16.0f;

    void prepare(double sampleRate, int hop) noexcept
    {
        const BandLayout layout;
        for (int b = 0; b < kNumBands; ++b)
        {
            const auto i = size_t(b);
            const double gBase = std::tan(kPi * layout.fc[i] / sampleRate);
            const auto d = svf::bandpass<double>(gBase, layout.qSc[i]);
            bank.a1[b] = float(d.c.a1);
            bank.a2[b] = float(d.c.a2);
            bank.a3[b] = float(d.c.a3);
            bank.m1[b] = float(d.c.m1);
            const double a = alphaS(std::max(0.002, 1.5 / layout.fc[i]), sampleRate);
            bank.aEnv[b] = float(a);
            bank.omaEnv[b] = float(1.0 - a);
        }
        aGlobal = float(alphaH(0.300, hop, sampleRate));
        aFloorDown = float(alphaH(0.050, hop, sampleRate));
        aFloorUp = float(alphaH(0.800, hop, sampleRate));
        aRing = float(alphaH(0.050, hop, sampleRate));
        reset();
    }

    /** Clears filter state and all running statistics (frame counter back to 0). */
    void reset() noexcept
    {
        bank.clearState();
        pGlobal = 0.0f;
        frame = 0;
        for (int b = 0; b < kNumBands; ++b)
            eFloor[b] = ePrev[b] = tRaw[b] = eS[b] = deltaE[b] = mdRaw[b] = 0.0f;
        for (int s = 0; s < kModSlots; ++s)
            for (int b = 0; b < kNumBands; ++b)
                slotMax[s][b] = slotMin[s][b] = 0.0f;
    }

    /** Clears only the filter state (RESET parameter). */
    void resetFilters() noexcept
    {
        for (int c = 0; c < kMaxChannels; ++c)
            for (int b = 0; b < kNumBands; ++b)
                bank.ic1[c][b] = bank.ic2[c][b] = 0.0f;
    }

    /** Per-sample update. Returns false when the input was not finite; the
        analyzer is then reset and the sample is treated as silence. */
    inline bool tick(float xL, float xR, bool stereo) noexcept
    {
        if (! (std::isfinite(xL) && std::isfinite(xR)))
        {
            reset();
            return false;
        }
        xL = std::min(kInputLimit, std::max(-kInputLimit, xL));
        xR = std::min(kInputLimit, std::max(-kInputLimit, xR));
        bank.tick(xL, xR, stereo);
        return true;
    }

    /** Hop boundary: builds the feature tensor [channel][band], 192 floats. */
    void computeFrame(float mode, float* features) noexcept
    {
        const float* ps = bank.ps;
        alignas(32) float amp[kNumBands];
        float sum = 0.0f;
        for (int b = 0; b < kNumBands; ++b)
        {
            eS[b] = 10.0f * math::log10(ps[b] + 1.0e-10f);
            amp[b] = std::sqrt(ps[b]);
            sum += ps[b];
        }

        pGlobal = (1.0f - aGlobal) * pGlobal + aGlobal * (sum * (1.0f / float(kNumBands)));
        const float eGlobal = std::max(-90.0f, 10.0f * math::log10(pGlobal + 1.0e-10f));

        if (frame == 0)
        {
            for (int b = 0; b < kNumBands; ++b)
                eFloor[b] = eS[b];
        }
        else
        {
            for (int b = 0; b < kNumBands; ++b)
            {
                const float a = eS[b] < eFloor[b] ? aFloorDown : aFloorUp;
                eFloor[b] = (1.0f - a) * eFloor[b] + a * eS[b];
            }
        }

        // Two-stage ring buffer: 15 slots of 16 frames (225 to 240 frames).
        const int slot = int((frame / kModBlockFrames) % kModSlots);
        const bool newBlock = (frame % kModBlockFrames) == 0;
        for (int b = 0; b < kNumBands; ++b)
        {
            slotMax[slot][b] = newBlock ? amp[b] : std::max(slotMax[slot][b], amp[b]);
            slotMin[slot][b] = newBlock ? amp[b] : std::min(slotMin[slot][b], amp[b]);
        }
        const int filled = int(std::min<std::uint64_t>(frame / kModBlockFrames + 1, kModSlots));
        for (int b = 0; b < kNumBands; ++b)
        {
            float hi = slotMax[0][b], lo = slotMin[0][b];
            for (int s = 1; s < filled; ++s)
            {
                hi = std::max(hi, slotMax[s][b]);
                lo = std::min(lo, slotMin[s][b]);
            }
            const float md = 20.0f * math::log10((hi + 1.0e-5f) / (lo + 1.0e-5f));
            mdRaw[b] = eS[b] < -80.0f ? 0.0f : md;
        }

        for (int b = 0; b < kNumBands; ++b)
        {
            const float neighbour = b == 0 ? eS[1]
                                  : b == kNumBands - 1 ? eS[kNumBands - 2]
                                  : 0.5f * (eS[b - 1] + eS[b + 1]);
            tRaw[b] = (1.0f - aRing) * tRaw[b] + aRing * (eS[b] - neighbour);
            deltaE[b] = frame == 0 ? 0.0f : eS[b] - ePrev[b];
        }

        auto clip = [](float v, float lo, float hi) noexcept { return std::min(hi, std::max(lo, v)); };
        for (int b = 0; b < kNumBands; ++b)
        {
            features[0 * kNumBands + b] = clip((eS[b] - eFloor[b]) / 30.0f, -3.0f, 3.0f);
            features[1 * kNumBands + b] = clip((eS[b] - eGlobal) / 20.0f, -3.0f, 3.0f);
            features[2 * kNumBands + b] = clip(deltaE[b] / 6.0f, -3.0f, 3.0f);
            features[3 * kNumBands + b] = clip((mdRaw[b] - 12.0f) / 12.0f, -2.0f, 4.0f);
            features[4 * kNumBands + b] = clip(tRaw[b] / 15.0f, -2.0f, 4.0f);
            features[5 * kNumBands + b] = mode;
            ePrev[b] = eS[b];
        }
        ++frame;
    }

    // Raw per-frame values (dB) shared with the FSM.
    const float* bandEnergyDb() const noexcept { return eS; }
    const float* deltaEnergyDb() const noexcept { return deltaE; }
    const float* modulationDb() const noexcept { return mdRaw; }
    const float* ringIndexDb() const noexcept { return tRaw; }
    std::uint64_t frameCount() const noexcept { return frame; }

private:
    SidechainBank bank;
    float aGlobal = 0.0f, aFloorDown = 0.0f, aFloorUp = 0.0f, aRing = 0.0f;
    float pGlobal = 0.0f;
    std::uint64_t frame = 0;
    alignas(32) float eS[kNumBands] {}, eFloor[kNumBands] {}, ePrev[kNumBands] {};
    alignas(32) float tRaw[kNumBands] {}, deltaE[kNumBands] {}, mdRaw[kNumBands] {};
    alignas(32) float slotMax[kModSlots][kNumBands] {}, slotMin[kModSlots][kNumBands] {};
};

} // namespace zerofb
