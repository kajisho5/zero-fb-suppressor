// ZERO-FB - ZeroLatencyTest (SPEC 7.2 item 2).
//
// Bit-perfect bypass, 0-sample latency, block-size independence with zero heap
// allocation, gain-coupling accuracy, the fail-safe guard and the FSM.

#include "TestUtil.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <new>

// ---------------------------------------------------------------------------
// Global allocation counter: every operator new in this executable is counted.
// ---------------------------------------------------------------------------
static std::atomic<long> gAllocations { 0 };

void* operator new(std::size_t size)
{
    gAllocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size == 0 ? 1 : size))
        return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace zerofb;
using namespace zerofbtest;

namespace {

const double kRates[] = { 44100.0, 48000.0, 96000.0 };

/** Runs a mono or stereo buffer through the engine in blocks of one size. */
void run(ZeroFbEngine& engine, const std::vector<float>* in, std::vector<float>* out, int numCh,
         size_t begin, size_t end, int block = 512)
{
    for (size_t pos = begin; pos < end;)
    {
        const int n = int(std::min<size_t>(size_t(block), end - pos));
        const float* ins[2] = { in[0].data() + pos, in[numCh - 1].data() + pos };
        float* outs[2] = { out[0].data() + pos, out[numCh - 1].data() + pos };
        engine.process(ins, outs, numCh, numCh, n);
        pos += size_t(n);
    }
}

void fillNoise(std::vector<float>& v, Noise& noise, float gain)
{
    for (auto& s : v)
        s = gain * noise.next();
}

// ---------------------------------------------------------------------------
void testBitPerfectBypass()
{
    section("bit-perfect bypass");
    for (double fs : kRates)
    {
        for (int numCh = 1; numCh <= 2; ++numCh)
        {
            for (int variant = 0; variant < 2; ++variant) // 0: BYPASS = ON, 1: SUPPRESS = 0 %
            {
                const size_t total = size_t(fs * 1.0);
                const size_t active = size_t(fs * 0.3);
                Noise noise;
                std::vector<float> in[2], out[2];
                for (int c = 0; c < 2; ++c)
                {
                    in[c].resize(total);
                    out[c].assign(total, 0.0f);
                    fillNoise(in[c], noise, 0.5f);
                }

                auto engine = std::make_unique<ZeroFbEngine>();
                PatternEstimator estimator;
                engine->setEstimator(&estimator);
                engine->prepare(fs, numCh);
                EngineParams p;
                engine->setParams(p);
                run(*engine, in, out, numCh, 0, active);
                ZFB_CHECK(engine->wet() == 1.0f);

                bool differs = false;
                for (size_t n = active / 2; n < active; ++n)
                    differs = differs || out[0][n] != in[0][n];
                ZFB_CHECK_MSG(differs, "active path must process the signal");

                if (variant == 0) p.bypass = true; else p.suppress = 0.0f;
                engine->setParams(p);

                // The crossfade takes exactly N_ramp samples.
                const size_t ramp = size_t(engine->rampSamples());
                ZFB_CHECK(ramp == size_t(std::lround(0.005 * fs)));
                run(*engine, in, out, numCh, active, active + ramp - 1, 1);
                ZFB_CHECK(engine->wet() > 0.0f);
                run(*engine, in, out, numCh, active + ramp - 1, active + ramp, 1);
                ZFB_CHECK(engine->wet() == 0.0f);

                run(*engine, in, out, numCh, active + ramp, total, 333);
                bool identical = true;
                for (int c = 0; c < numCh; ++c)
                    identical = identical
                        && std::memcmp(in[c].data() + active + ramp, out[c].data() + active + ramp,
                                       (total - active - ramp) * sizeof(float)) == 0;
                ZFB_CHECK_MSG(identical, fmt("fs=%.0f ch=%.0f variant=%.0f", fs, numCh, variant));

                // The transition itself has no jump larger than the signal allows.
                float maxStep = 0.0f;
                for (size_t n = active; n < active + ramp; ++n)
                    maxStep = std::max(maxStep, std::abs((out[0][n] - in[0][n]) - (out[0][n - 1] - in[0][n - 1])));
                ZFB_CHECK_MSG(maxStep < 1.0f, fmt("crossfade step %.4f", maxStep));
            }
        }
    }

    // A host that loads the plug-in already bypassed gets the input back from sample 0.
    {
        const size_t total = 4096;
        Noise noise;
        std::vector<float> in[2], out[2];
        for (int c = 0; c < 2; ++c) { in[c].resize(total); out[c].assign(total, 0.0f); fillNoise(in[c], noise, 0.9f); }
        auto engine = std::make_unique<ZeroFbEngine>();
        engine->prepare(48000.0, 2);
        EngineParams p;
        p.bypass = true;
        engine->setParams(p);
        run(*engine, in, out, 2, 0, total, 64);
        ZFB_CHECK(std::memcmp(in[0].data(), out[0].data(), total * sizeof(float)) == 0);
        ZFB_CHECK(std::memcmp(in[1].data(), out[1].data(), total * sizeof(float)) == 0);
    }
}

// ---------------------------------------------------------------------------
void testBlockSizeIndependence()
{
    section("block sizes 1/7/16/31/64/5000 and zero heap allocation");
    const int blockSizes[] = { 1, 7, 16, 31, 64, 5000 };
    for (double fs : kRates)
    {
        const size_t total = size_t(fs * 2.0);
        Noise noise;
        std::vector<float> in[2], ref[2], out[2];
        for (int c = 0; c < 2; ++c)
        {
            in[c].resize(total);
            for (size_t n = 0; n < total; ++n)
                in[c][n] = 0.2f * noise.next()
                         + 0.3f * float(std::sin(2.0 * kPi * 1000.0 * double(n) / fs + c));
            ref[c].assign(total, 0.0f);
            out[c] = in[c]; // processed in place below
        }

        auto reference = std::make_unique<ZeroFbEngine>();
        auto engine = std::make_unique<ZeroFbEngine>();
        PatternEstimator estRef, est;
        reference->setEstimator(&estRef);
        engine->setEstimator(&est);
        reference->prepare(fs, 2);
        engine->prepare(fs, 2); // no maximum block size is announced: 5000 must just work
        EngineParams p;
        p.floorDb = -36.0f;
        p.suppress = 1.0f;
        reference->setParams(p);
        engine->setParams(p);

        const long before = gAllocations.load();
        run(*reference, in, ref, 2, 0, total, 1);
        size_t pos = 0;
        int index = 0;
        while (pos < total)
        {
            const size_t n = std::min<size_t>(size_t(blockSizes[index % 6]), total - pos);
            run(*engine, out, out, 2, pos, pos + n, int(n)); // in == out
            pos += n;
            ++index;
        }
        const long after = gAllocations.load();
        ZFB_CHECK_MSG(after == before, fmt("heap allocations inside process: %.0f", double(after - before)));

        double maxDiff = 0.0, energy = 0.0;
        for (int c = 0; c < 2; ++c)
            for (size_t n = 0; n < total; ++n)
            {
                maxDiff = std::max(maxDiff, double(std::abs(out[c][n] - ref[c][n])));
                energy += double(ref[c][n]) * double(ref[c][n]);
            }
        ZFB_CHECK_MSG(maxDiff < 1.0e-6, fmt("fs=%.0f max diff %.3e", fs, maxDiff));
        ZFB_CHECK(energy > 1.0);
    }
}

// ---------------------------------------------------------------------------
void testZeroLatencyAndNoPreRinging()
{
    section("0-sample onset and no pre-ringing");
    for (double fs : kRates)
    {
        const size_t impulseAt = size_t(fs * 0.5) + 5;
        const size_t total = impulseAt + 2048;
        std::vector<float> in[2], out[2];
        for (int c = 0; c < 2; ++c) { in[c].assign(total, 0.0f); out[c].assign(total, 1.0f); }
        in[0][impulseAt] = 0.5f;
        in[1][impulseAt] = -0.25f;

        auto engine = std::make_unique<ZeroFbEngine>();
        PatternEstimator estimator; // arbitrary, time-varying masks
        engine->setEstimator(&estimator);
        engine->prepare(fs, 2);
        EngineParams p;
        p.suppress = 1.0f;
        p.dereverb = 1.0f;
        p.floorDb = -36.0f;
        engine->setParams(p);
        run(*engine, in, out, 2, 0, total, 97);

        bool silentBefore = true;
        for (int c = 0; c < 2; ++c)
            for (size_t n = 0; n < impulseAt; ++n)
                silentBefore = silentBefore && out[c][n] == 0.0f;
        ZFB_CHECK_MSG(silentBefore, "output before the impulse must be exactly 0");
        ZFB_CHECK_MSG(std::abs(out[0][impulseAt]) > 1.0e-3f && std::abs(out[1][impulseAt]) > 1.0e-3f,
                      fmt("impulse response starts at the same sample (%.5f, %.5f)", out[0][impulseAt], out[1][impulseAt]));
        ZFB_CHECK(out[0][impulseAt] > 0.0f && out[1][impulseAt] < 0.0f);
        ZFB_CHECK(ZeroFbEngine::latencySamples() == 0);
    }
}

// ---------------------------------------------------------------------------
/** Steady-state gain of the engine at frequency f, in dB, relative to the input. */
double steadyGainDb(double fs, double f, const float* maskFb, float floorDb)
{
    const size_t settle = size_t(fs * 0.35);
    const size_t periods = size_t(std::floor(0.25 * f));
    const size_t measure = size_t(std::llround(double(periods) * fs / f));
    const size_t total = settle + measure;
    std::vector<float> in[2], out[2];
    in[0].resize(total);
    for (size_t n = 0; n < total; ++n)
        in[0][n] = 0.1f * float(std::sin(2.0 * kPi * f * double(n) / fs));
    in[1] = in[0];
    out[0].assign(total, 0.0f);
    out[1].assign(total, 0.0f);

    auto engine = std::make_unique<ZeroFbEngine>();
    FixedMaskEstimator estimator;
    for (int b = 0; b < kNumBands; ++b)
        estimator.fb[b] = maskFb[b];
    engine->setEstimator(&estimator);
    engine->prepare(fs, 1);
    EngineParams p;
    p.suppress = 1.0f;
    p.dereverb = 0.0f;
    p.floorDb = floorDb;
    engine->setParams(p);
    run(*engine, in, out, 1, 0, total);
    return dbFromLinear(toneAmplitude(out[0], settle, total, f, fs) / toneAmplitude(in[0], settle, total, f, fs));
}

void testGainCoupling()
{
    section("gain coupling: uniform -12 dB and single band -12 dB");
    const BandLayout layout;
    const float cut = std::pow(10.0f, -12.0f / 20.0f);
    for (double fs : kRates)
    {
        float unity[kNumBands], uniform[kNumBands], single[kNumBands];
        for (int b = 0; b < kNumBands; ++b) { unity[b] = 1.0f; uniform[b] = cut; single[b] = 1.0f; }
        single[15] = cut;

        double worst = 0.0, worstRaw = 0.0;
        int worstBand = 0;
        for (int b = 0; b < kNumBands; ++b)
        {
            const double f = layout.fc[size_t(b)];
            // The reference run has all masks at 1: it contains the DC blocker
            // only, so the difference is the gain of Stage 6A.
            const double base = steadyGainDb(fs, f, unity, -36.0f);
            const double raw = steadyGainDb(fs, f, uniform, -36.0f);
            const double err = (raw - base) + 12.0;
            if (std::abs(err) > std::abs(worst)) { worst = err; worstBand = b; }
            if (std::abs(raw + 12.0) > std::abs(worstRaw)) worstRaw = raw + 12.0;
            ZFB_CHECK_MSG(std::abs(err) <= 1.0, fmt("fs=%.0f band %.0f (%.1f Hz): EQ gain error %.3f dB", fs, b, f, err));
            // SPEC 7.2: output / input for a steady sine, DC blocker included.
            ZFB_CHECK_MSG(std::abs(raw + 12.0) <= 1.0, fmt("fs=%.0f band %.0f (%.1f Hz): input-referred gain %.3f dB", fs, b, f, raw));
        }
        std::printf("  fs=%.0f uniform: worst EQ error %+.3f dB at band %d, worst input-referred error %+.3f dB\n",
                    fs, worst, worstBand, worstRaw);

        const double f15 = layout.fc[15];
        const double single15 = steadyGainDb(fs, f15, single, -36.0f) - steadyGainDb(fs, f15, unity, -36.0f);
        std::printf("  fs=%.0f single band 15: %+.3f dB\n", fs, single15);
        ZFB_CHECK_MSG(std::abs(single15 + 12.0) <= 0.5, fmt("fs=%.0f single band 15: %.3f dB", fs, single15));
    }
}

// ---------------------------------------------------------------------------
void testFailsafe()
{
    section("NaN / Inf / over-range fail-safe");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const float plus30dBFS = std::pow(10.0f, 30.0f / 20.0f);
    const float injections[] = { nan, inf, -inf, plus30dBFS };

    for (double fs : kRates)
    {
        for (float bad : injections)
        {
            auto engine = std::make_unique<ZeroFbEngine>();
            FixedMaskEstimator estimator;
            engine->setEstimator(&estimator);
            engine->prepare(fs, 2);
            EngineParams p;
            engine->setParams(p);
            const int hop = engine->hopLength();
            const int ramp = engine->rampSamples();
            const float floorLin = std::pow(10.0f, p.floorDb / 20.0f);

            const size_t injectAt = size_t(fs * 0.2) / size_t(hop) * size_t(hop) + 3; // not on a hop boundary
            const size_t total = injectAt + size_t(fs * 0.3);
            std::vector<float> in[2], out[2];
            for (int c = 0; c < 2; ++c)
            {
                in[c].resize(total);
                for (size_t n = 0; n < total; ++n)
                    in[c][n] = 0.2f * float(std::sin(2.0 * kPi * 440.0 * double(n) / fs));
                out[c].assign(total, 0.0f);
            }
            in[0][injectAt] = bad;

            run(*engine, in, out, 2, 0, injectAt, 64);
            ZFB_CHECK(engine->failsafeTrips() == 0);
            const int resetsBefore = estimator.resets;

            run(*engine, in, out, 2, injectAt, injectAt + 1, 1);
            ZFB_CHECK_MSG(out[0][injectAt] == 0.0f && out[1][injectAt] == 0.0f, "muted on the offending sample");
            ZFB_CHECK(engine->failsafeTrips() == 1);
            ZFB_CHECK(estimator.resets == resetsBefore + 1);
            bool atFloor = true;
            for (int b = 0; b < kNumBands; ++b)
                atFloor = atFloor && engine->bandMasks()[b] == floorLin;
            ZFB_CHECK_MSG(atFloor, "all masks restart from the floor");
            ZFB_CHECK(engine->failsafeRecovering());

            // Linear fade-in over N_ramp samples, then normal operation.
            run(*engine, in, out, 2, injectAt + 1, injectAt + 1 + size_t(ramp), 1);
            ZFB_CHECK(! engine->failsafeRecovering());
            ZFB_CHECK(engine->failsafeTrips() == 1);
            run(*engine, in, out, 2, injectAt + 1 + size_t(ramp), total, 64);

            bool finite = true;
            float peakAfter = 0.0f, peakEarly = 0.0f;
            for (int c = 0; c < 2; ++c)
                for (size_t n = 0; n < total; ++n)
                    finite = finite && std::isfinite(out[c][n]);
            for (size_t n = injectAt + 1; n < injectAt + 1 + size_t(ramp) / 8; ++n)
                peakEarly = std::max(peakEarly, std::abs(out[0][n]));
            for (size_t n = total - size_t(fs * 0.05); n < total; ++n)
                peakAfter = std::max(peakAfter, std::abs(out[0][n]));
            ZFB_CHECK_MSG(finite, "output stays finite");
            ZFB_CHECK_MSG(peakEarly < 0.2f * floorLin, fmt("fade starts near silence (%.5f)", peakEarly));
            ZFB_CHECK_MSG(peakAfter > 0.15f && peakAfter < 0.25f, fmt("signal is back after recovery (%.4f)", peakAfter));
            ZFB_CHECK(engine->failsafeTrips() == 1);

            bool featuresFinite = true;
            for (int i = 0; i < kFeatureSize; ++i)
                featuresFinite = featuresFinite && std::isfinite(engine->lastFeatures()[i]);
            ZFB_CHECK(featuresFinite);
        }
    }

    // A non-finite sample during bypass passes through untouched, and the
    // control path is clean again afterwards.
    {
        auto engine = std::make_unique<ZeroFbEngine>();
        engine->prepare(48000.0, 1);
        EngineParams p;
        p.bypass = true;
        engine->setParams(p);
        std::vector<float> in[2], out[2];
        in[0].assign(9600, 0.1f);
        in[0][1000] = nan;
        in[1] = in[0];
        out[0].assign(9600, 0.0f);
        out[1] = out[0];
        run(*engine, in, out, 1, 0, 9600, 128);
        ZFB_CHECK(std::memcmp(in[0].data(), out[0].data(), 9600 * sizeof(float)) == 0);
        bool featuresFinite = true;
        for (int i = 0; i < kFeatureSize; ++i)
            featuresFinite = featuresFinite && std::isfinite(engine->lastFeatures()[i]);
        ZFB_CHECK(featuresFinite);
    }
}

// ---------------------------------------------------------------------------
void testLimiterAndLayouts()
{
    section("soft limiter, Mono -> Stereo, unsupported sample rate");
    {
        const double fs = 48000.0;
        const size_t total = 48000;
        std::vector<float> in[2], out[2];
        in[0].resize(total);
        for (size_t n = 0; n < total; ++n)
            in[0][n] = 4.0f * float(std::sin(2.0 * kPi * 997.0 * double(n) / fs));
        in[1] = in[0];
        out[0].assign(total, 0.0f);
        out[1] = out[0];
        auto engine = std::make_unique<ZeroFbEngine>();
        engine->prepare(fs, 1);
        engine->setParams(EngineParams {});
        run(*engine, in, out, 1, 0, total);
        float peak = 0.0f;
        for (float s : out[0]) peak = std::max(peak, std::abs(s));
        ZFB_CHECK_MSG(peak <= ZeroFbEngine::kLimiterCeiling + 1.0e-5f && peak > 0.9f, fmt("limited peak %.6f", peak));
        ZFB_CHECK(engine->failsafeTrips() == 0);
    }
    {
        Noise noise;
        std::vector<float> in(4800), outL(4800), outR(4800, 9.0f);
        fillNoise(in, noise, 0.3f);
        auto engine = std::make_unique<ZeroFbEngine>();
        engine->prepare(48000.0, 1);
        engine->setParams(EngineParams {});
        const float* ins[1] = { in.data() };
        float* outs[2] = { outL.data(), outR.data() };
        engine->process(ins, outs, 1, 2, 4800);
        ZFB_CHECK(std::memcmp(outL.data(), outR.data(), 4800 * sizeof(float)) == 0);
        ZFB_CHECK(outL[100] != in[100]);
    }
    {
        Noise noise;
        std::vector<float> in[2], out[2];
        for (int c = 0; c < 2; ++c) { in[c].resize(4410); out[c].assign(4410, 0.0f); fillNoise(in[c], noise, 0.3f); }
        auto engine = std::make_unique<ZeroFbEngine>();
        engine->prepare(88200.0, 2);
        engine->setParams(EngineParams {});
        ZFB_CHECK(! engine->isSupported());
        run(*engine, in, out, 2, 0, 4410, 100);
        ZFB_CHECK(std::memcmp(in[0].data(), out[0].data(), 4410 * sizeof(float)) == 0);
        ZFB_CHECK(std::memcmp(in[1].data(), out[1].data(), 4410 * sizeof(float)) == 0);
    }
}

// ---------------------------------------------------------------------------
struct SmootherRig
{
    MaskSmoother smoother;
    float fb[kNumBands], rev[kNumBands], deltaE[kNumBands], md[kNumBands], ring[kNumBands];
    MaskSmoother::Inputs in;

    SmootherRig()
    {
        smoother.prepare(48000.0, 16);
        for (int b = 0; b < kNumBands; ++b) { fb[b] = 1.0f; rev[b] = 1.0f; deltaE[b] = 0.0f; md[b] = 0.0f; ring[b] = 0.0f; }
        in.mFb = fb; in.mRev = rev; in.deltaE = deltaE; in.mdRaw = md; in.tRaw = ring;
        in.suppress = 1.0f; in.dereverb = 0.0f; in.floorDb = -36.0f; in.vocal = false;
    }
    void frames(int n) { for (int i = 0; i < n; ++i) smoother.update(in); }
    int secs(double s) const { return int(std::lround(s * 3000.0)); }
};

void testStateMachine()
{
    section("hysteresis FSM");
    using State = MaskSmoother::State;
    const int band = 12;

    // One continuous feedback event must stay at back-off level 0.
    {
        SmootherRig r;
        r.fb[band] = 0.1f;
        r.ring[band] = 20.0f;
        r.frames(r.secs(5.0));
        ZFB_CHECK(r.smoother.bandState(band) == State::hold);
        ZFB_CHECK_MSG(r.smoother.bandBackoffLevel(band) == 0, "a single event does not escalate itself");
        ZFB_CHECK(std::abs(r.smoother.current()[band] - 0.1f) < 1.0e-3f);
        ZFB_CHECK(r.smoother.current()[band - 1] == 1.0f);
    }

    // Hold freezes the gain, then slow release; a recurrence escalates 0 -> 1 -> 2.
    {
        SmootherRig r;
        r.fb[band] = 0.1f;
        r.ring[band] = 20.0f;
        r.frames(r.secs(0.1));
        const float held = r.smoother.current()[band];
        r.fb[band] = 1.0f;
        r.frames(r.smoother.holdFrames(0) - 10);
        ZFB_CHECK(r.smoother.bandState(band) == State::hold);
        ZFB_CHECK_MSG(r.smoother.current()[band] == held, "gain rise is frozen during hold");
        r.frames(20);
        ZFB_CHECK(r.smoother.bandState(band) == State::slowRelease);
        ZFB_CHECK(r.smoother.current()[band] > held && r.smoother.current()[band] < 0.2f);

        r.fb[band] = 0.1f; // recurrence during slow release, with a ringing peak
        r.frames(r.secs(0.1));
        ZFB_CHECK(r.smoother.bandState(band) == State::hold);
        ZFB_CHECK(r.smoother.bandBackoffLevel(band) == 1);
        ZFB_CHECK(r.smoother.bandHoldRemaining(band) > r.smoother.holdFrames(1) - r.secs(0.1)
                  && r.smoother.bandHoldRemaining(band) <= r.smoother.holdFrames(1));

        r.fb[band] = 1.0f;
        r.frames(r.smoother.holdFrames(1) + 5);
        ZFB_CHECK(r.smoother.bandState(band) == State::slowRelease);
        r.fb[band] = 0.1f;
        r.frames(r.secs(0.1));
        ZFB_CHECK(r.smoother.bandBackoffLevel(band) == 2);
        ZFB_CHECK(r.smoother.bandHoldRemaining(band) > r.smoother.holdFrames(2) - r.secs(0.1)
                  && r.smoother.bandHoldRemaining(band) <= r.smoother.holdFrames(2));

        // Level 2 is the maximum.
        r.fb[band] = 1.0f;
        r.frames(r.smoother.holdFrames(2) + 5);
        r.fb[band] = 0.1f;
        r.frames(r.secs(0.1));
        ZFB_CHECK(r.smoother.bandBackoffLevel(band) == 2);
    }

    // No ringing peak (T_raw <= 6 dB): a recurrence does not escalate.
    {
        SmootherRig r;
        r.fb[band] = 0.1f;
        r.frames(r.secs(0.1));
        r.fb[band] = 1.0f;
        r.frames(r.smoother.holdFrames(0) + 5);
        r.fb[band] = 0.1f;
        r.frames(r.secs(0.1));
        ZFB_CHECK(r.smoother.bandBackoffLevel(band) == 0);
    }

    // At most 4 bands may be in a long hold at the same time.
    {
        SmootherRig r;
        const int bands[] = { 3, 7, 11, 15, 19, 23 };
        for (int b : bands) { r.fb[b] = 0.1f; r.ring[b] = 20.0f; }
        r.frames(r.secs(0.1));
        for (int b : bands) r.fb[b] = 1.0f;
        r.frames(r.smoother.holdFrames(0) + 5);
        for (int b : bands) r.fb[b] = 0.1f;
        r.frames(r.secs(0.1));
        int longHolds = 0;
        for (int b : bands) longHolds += r.smoother.bandBackoffLevel(b) >= 1 ? 1 : 0;
        ZFB_CHECK_MSG(longHolds == 4, fmt("long holds: %.0f", longHolds));
    }

    // After the slow release reaches -1 dB the band is back to normal and the
    // 2 s re-offence window expires.
    {
        SmootherRig r;
        r.fb[band] = 0.1f;
        r.ring[band] = 20.0f;
        r.frames(r.secs(0.1));
        r.fb[band] = 1.0f;
        r.frames(r.smoother.holdFrames(0) + r.secs(6.0));
        ZFB_CHECK(r.smoother.bandState(band) == State::normalRelease);
        ZFB_CHECK(r.smoother.current()[band] == 1.0f);
        r.fb[band] = 0.1f;
        r.frames(r.secs(0.1));
        ZFB_CHECK(r.smoother.bandBackoffLevel(band) == 0);
    }

    // Voice-triggered release: hold is dropped and the gain is back above
    // -1 dB within 20 ms once Head A is safe again.
    {
        SmootherRig r;
        r.in.floorDb = -24.0f;
        r.fb[band] = 0.03f; // clamps at the floor
        r.frames(r.secs(0.2));
        ZFB_CHECK(r.smoother.bandState(band) == State::hold);
        r.fb[band] = 1.0f;
        r.frames(r.secs(0.05));
        ZFB_CHECK(r.smoother.bandState(band) == State::hold);
        for (int b = 8; b <= 12; ++b) { r.deltaE[b] = 6.0f; r.md[b] = 15.0f; }
        r.frames(1);
        for (int b = 8; b <= 12; ++b) { r.deltaE[b] = 0.0f; }
        ZFB_CHECK(r.smoother.bandState(band) == State::normalRelease);
        r.frames(r.secs(0.020) - 1);
        const double gainDb = dbFromLinear(r.smoother.current()[band]);
        ZFB_CHECK_MSG(gainDb >= -1.0, fmt("gain 20 ms after the voice trigger: %.2f dB", gainDb));
    }

    // A band whose Head A still reports feedback keeps its hold during a voice trigger.
    {
        SmootherRig r;
        r.fb[band] = 0.1f;
        r.frames(r.secs(0.2));
        r.fb[band] = 0.6f; // not deep, not safe (<= 0.75)
        for (int b = 8; b <= 12; ++b) { r.deltaE[b] = 6.0f; r.md[b] = 15.0f; }
        r.frames(30);
        ZFB_CHECK(r.smoother.bandState(band) == State::hold);
    }

    // RESET: 5 ms forced ramp to exactly 1.0, all timers cleared.
    {
        SmootherRig r;
        r.fb[band] = 0.1f;
        r.frames(r.secs(0.2));
        r.fb[band] = 1.0f;
        r.smoother.triggerReset();
        ZFB_CHECK(r.smoother.resetRampFrames() == 15);
        r.frames(r.smoother.resetRampFrames() - 1);
        ZFB_CHECK(r.smoother.current()[band] < 1.0f && r.smoother.resetRampActive());
        r.frames(1);
        ZFB_CHECK(r.smoother.current()[band] == 1.0f && ! r.smoother.resetRampActive());
        ZFB_CHECK(r.smoother.bandState(band) == State::normalRelease);
        ZFB_CHECK(r.smoother.bandHoldRemaining(band) == 0);
    }

    // Engine-level RESET clears the estimator and returns to unity gain in 5 ms.
    {
        auto engine = std::make_unique<ZeroFbEngine>();
        FixedMaskEstimator estimator;
        estimator.setAll(0.2f, 1.0f);
        engine->setEstimator(&estimator);
        engine->prepare(48000.0, 1);
        EngineParams p;
        p.suppress = 1.0f;
        engine->setParams(p);
        std::vector<float> in[2], out[2];
        in[0].assign(48000, 0.0f); in[1] = in[0]; out[0] = in[0]; out[1] = in[0];
        run(*engine, in, out, 1, 0, 9600);
        ZFB_CHECK(engine->bandGainsDb()[10] < -3.0f);
        estimator.setAll(1.0f, 1.0f);
        const int resets = estimator.resets;
        engine->requestReset();
        run(*engine, in, out, 1, 9600, 9600 + 16 * 16, 16);
        ZFB_CHECK(estimator.resets == resets + 1);
        ZFB_CHECK(engine->bandMasks()[10] == 1.0f);
        ZFB_CHECK(engine->bandGainsDb()[10] == 0.0f);
    }
}

} // namespace

int main()
{
    testBitPerfectBypass();
    testBlockSizeIndependence();
    testZeroLatencyAndNoPreRinging();
    testGainCoupling();
    testFailsafe();
    testLimiterAndLayouts();
    testStateMachine();
    return finish("ZeroLatencyTest");
}
