// ZERO-FB - complete real-time pipeline (SPEC chapter 2), independent of JUCE.
//
// The engine processes sample by sample, so any host block length (including
// blocks larger than the one announced at prepare time) is handled without
// work buffers, chunking or allocation. Signal path latency is 0 samples.
#pragma once

#include "MaskSmoother.h"
#include "SerialPeakingEq.h"
#include "SidechainAnalyzer.h"

#include <atomic>
#include <cstdint>

namespace zerofb {

/** Source of the two 32-band masks (Stage 4). The neural engine implements
    this; tests can supply their own. infer() runs on the audio thread. */
class MaskEstimator
{
public:
    virtual ~MaskEstimator() = default;
    virtual void reset() noexcept = 0;
    /** features: 192 floats [channel][band]. masks: 64 floats, fb[32] then rev[32]. */
    virtual void infer(const float* features, float* masks) noexcept = 0;
};

struct EngineParams
{
    float suppress = 0.75f; // S, 0 .. 1
    float dereverb = 0.5f;  // D, 0 .. 1
    float floorDb = -24.0f; // -36 .. -6
    bool vocal = false;     // MODE
    bool bypass = false;
};

class ZeroFbEngine
{
public:
    static constexpr float kOverRange = 16.0f;       // +24 dBFS
    static constexpr float kLimiterCeiling = 0.944060876f; // -0.5 dBFS
    static constexpr float kLimiterKnee = 0.85f;
    static constexpr double kDcBlockHz = 20.0;

    /** numChannels is the number of processed channels (1 or 2). An
        unsupported sample rate puts the engine into pass-through. */
    void prepare(double sampleRate, int numChannels) noexcept
    {
        channels = std::min(kMaxChannels, std::max(1, numChannels));
        hop = hopForSampleRate(sampleRate);
        supported = hop > 0;
        if (! supported)
            return;

        rampLength = std::max(1, int(std::lround(0.005 * sampleRate)));
        rampStep = 1.0f / float(rampLength);
        const double g = std::tan(kPi * kDcBlockHz / sampleRate);
        dcG = float(g / (1.0 + g));

        analyzer.prepare(sampleRate, hop);
        smoother.prepare(sampleRate, hop);
        eq.prepare(sampleRate, hop);
        if (estimator != nullptr)
            estimator->reset();

        hopPhase = 0;
        sampleCounter = 0;
        rampPos = rampLength;
        wetMix = 1.0f;
        firstSample = true;
        signalIdle = false;
        failsafeMuteRem = 0;
        failsafeCount = 0;
        resetTrigger.store(false);
        clearSignalState();
    }

    void setEstimator(MaskEstimator* e) noexcept { estimator = e; }
    void setParams(const EngineParams& p) noexcept { params = p; }
    /** RESET parameter. Safe to call from any thread. */
    void requestReset() noexcept { resetTrigger.store(true); }

    /** in/out may alias. numIn is 1 or 2; numOut >= numIn duplicates the last
        processed channel into the extra outputs (Mono -> Stereo). */
    void process(const float* const* in, float* const* out, int numIn, int numOut, int numSamples) noexcept
    {
        const int nCh = std::min(channels, numIn);
        if (! supported || nCh < 1)
        {
            for (int n = 0; n < numSamples; ++n)
                for (int c = 0; c < numOut; ++c)
                    out[c][n] = in[std::min(c, numIn - 1)][n];
            return;
        }

        const bool stereo = nCh == 2;
        const bool wantBypass = params.bypass || params.suppress <= 0.0f;
        if (firstSample)
        {
            rampPos = wantBypass ? 0 : rampLength; // start in the requested state, no ramp
            firstSample = false;
        }

        for (int n = 0; n < numSamples; ++n)
        {
            float x[kMaxChannels] { in[0][n], stereo ? in[1][n] : 0.0f };
            float y[kMaxChannels] { x[0], x[1] };

            // wet_mix moves by 1 / N_ramp per sample. An integer position keeps
            // both end points exact: 0.0 and 1.0 are reached in N_ramp samples.
            rampPos = std::min(rampLength, std::max(0, rampPos + (wantBypass ? -1 : 1)));
            wetMix = rampPos == rampLength ? 1.0f : float(rampPos) * rampStep;
            bool controlValid = true;

            if (wantBypass && wetMix == 0.0f)
            {
                // Steady bypass: bit-perfect copy, Stage 1 / 6A / 7B not run.
                if (! signalIdle)
                {
                    clearSignalState();
                    signalIdle = true;
                }
            }
            else
            {
                signalIdle = false;
                float w[kMaxChannels];
                for (int c = 0; c < nCh; ++c)
                {
                    // Stage 1: DC blocker (one-pole trapezoidal high-pass).
                    const float v = (x[c] - dcState[c]) * dcG;
                    const float lp = v + dcState[c];
                    dcState[c] = lp + v;
                    w[c] = x[c] - lp;
                }

                eq.tick(w, nCh, hopPhase); // Stage 6A

                // Stage 7A: fail-safe guard.
                bool bad = false;
                for (int c = 0; c < nCh; ++c)
                    bad = bad || ! std::isfinite(w[c]) || std::abs(w[c]) > kOverRange;
                if (bad)
                {
                    tripFailsafe();
                    controlValid = false;
                    for (int c = 0; c < nCh; ++c)
                        w[c] = 0.0f;
                }
                else if (failsafeMuteRem > 0)
                {
                    const float fade = float(rampLength - failsafeMuteRem + 1) * rampStep;
                    for (int c = 0; c < nCh; ++c)
                        w[c] *= fade;
                    --failsafeMuteRem;
                }

                // Stage 7B: soft limiter, then the crossfade mixer.
                for (int c = 0; c < nCh; ++c)
                {
                    const float mag = std::abs(w[c]);
                    if (mag > kLimiterKnee)
                    {
                        const float range = kLimiterCeiling - kLimiterKnee;
                        const float shaped = kLimiterKnee + range * math::tanh((mag - kLimiterKnee) / range);
                        w[c] = w[c] < 0.0f ? -shaped : shaped;
                    }
                    y[c] = wetMix == 1.0f ? w[c] : (1.0f - wetMix) * x[c] + wetMix * w[c];
                }
            }

            // Control path sample update (runs during bypass as well).
            if (controlValid)
            {
                if (! analyzer.tick(x[0], x[1], stereo) && estimator != nullptr)
                    estimator->reset();
            }

            if (hopPhase == hop - 1)
            {
                hopBoundary();
                hopPhase = 0;
            }
            else
            {
                ++hopPhase;
            }
            ++sampleCounter;

            for (int c = 0; c < numOut; ++c)
                out[c][n] = y[std::min(c, nCh - 1)];
        }
    }

    // Read-only state for meters and tests (audio thread values).
    const float* bandGainsDb() const noexcept { return eq.bandGainsDb(); }
    const float* bandMasks() const noexcept { return smoother.current(); }
    const MaskSmoother& maskSmoother() const noexcept { return smoother; }
    const SidechainAnalyzer& sidechain() const noexcept { return analyzer; }
    const SerialPeakingEq& equalizer() const noexcept { return eq; }
    const float* lastFeatures() const noexcept { return features; }
    float wet() const noexcept { return wetMix; }
    int hopLength() const noexcept { return hop; }
    int rampSamples() const noexcept { return rampLength; }
    bool isSupported() const noexcept { return supported; }
    int failsafeTrips() const noexcept { return failsafeCount; }
    bool failsafeRecovering() const noexcept { return failsafeMuteRem > 0; }
    static constexpr int latencySamples() noexcept { return 0; }

private:
    void clearSignalState() noexcept
    {
        dcState[0] = dcState[1] = 0.0f;
        eq.clearState();
    }

    void hopBoundary() noexcept
    {
        analyzer.computeFrame(params.vocal ? 1.0f : 0.0f, features);

        float masks[kMaskSize];
        if (estimator != nullptr)
        {
            estimator->infer(features, masks);
        }
        else
        {
            for (int i = 0; i < kMaskSize; ++i)
                masks[i] = 1.0f;
        }

        if (resetTrigger.exchange(false))
        {
            if (estimator != nullptr)
                estimator->reset();
            analyzer.resetFilters();
            clearSignalState();
            smoother.triggerReset();
        }

        MaskSmoother::Inputs in;
        in.mFb = masks;
        in.mRev = masks + kNumBands;
        in.deltaE = analyzer.deltaEnergyDb();
        in.mdRaw = analyzer.modulationDb();
        in.tRaw = analyzer.ringIndexDb();
        in.suppress = params.suppress;
        in.dereverb = params.dereverb;
        in.floorDb = params.floorDb;
        in.vocal = params.vocal;
        smoother.update(in);

        float gEq[kNumBands];
        solveGains(gEq);
        eq.setTargetGainsDb(gEq);
    }

    void solveGains(float* gEq) const noexcept
    {
        float gTarget[kNumBands];
        const float* m = smoother.current();
        for (int b = 0; b < kNumBands; ++b)
            gTarget[b] = std::min(0.0f, std::max(params.floorDb, 20.0f * math::log10(std::max(m[b], 1.0e-5f))));
        eq.solveCoupling(gTarget, params.floorDb, gEq);
    }

    void tripFailsafe() noexcept
    {
        ++failsafeCount;
        clearSignalState();
        analyzer.reset();
        if (estimator != nullptr)
            estimator->reset();
        smoother.forceFloor(params.floorDb);
        float gEq[kNumBands];
        solveGains(gEq);
        eq.setImmediate(gEq);
        failsafeMuteRem = rampLength;
    }

    SidechainAnalyzer analyzer;
    MaskSmoother smoother;
    SerialPeakingEq eq;
    MaskEstimator* estimator = nullptr;
    EngineParams params;
    std::atomic<bool> resetTrigger { false };

    int channels = 2, hop = 0, hopPhase = 0, rampLength = 1, rampPos = 1;
    bool supported = false, firstSample = true, signalIdle = false;
    float rampStep = 1.0f, wetMix = 1.0f, dcG = 0.0f;
    float dcState[kMaxChannels] {};
    int failsafeMuteRem = 0, failsafeCount = 0;
    std::uint64_t sampleCounter = 0;
    alignas(32) float features[kFeatureSize] {};
};

} // namespace zerofb
