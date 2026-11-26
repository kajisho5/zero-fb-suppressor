// ZERO-FB - ClosedLoopBench (SPEC 7.1).
//
// The plug-in sits inside a simulated acoustic loop:
//
//     x_mic[n] = s_voice[n] + G * (h_room * y_plugin)[n - D]
//
// For each of three virtual rooms the bench finds the gain G0 at which the
// loop diverges with the plug-in bypassed, then the highest gain that stays
// stable with the plug-in active. GBF (gain before feedback) = G_max - G0.
// It also counts hunting bursts and measures the mode-hopping settle time.
//
// Step 2 status: this is the bench skeleton. There is no trained model yet,
// so the GBF / hunting / hopping figures are printed for information only.
// What the bench does verify, and fail on, is its own machinery:
//   - the partitioned convolver matches a direct convolution,
//   - the measured G0 agrees with the G0 predicted from the room response,
//   - an engine with all masks at 1 gives no gain before feedback.
//
// The stimulus is synthetic speech-like audio; the Val / Test speech set of
// SPEC 7.1 replaces it in Step 3.
//
// Usage: ClosedLoopBench [--quick]

#include "TestUtil.h"
#include "../Source/nn/CausalGruEngine.h"
#include "nn/EmbeddedWeights.h" // generated: fixed test weights, not a trained model

#include <complex>
#include <cstring>
#include <memory>

using namespace zerofb;
using namespace zerofbtest;

namespace {

constexpr double kFs = 48000.0;
constexpr int kHop = 16;
constexpr double kRunSeconds = 10.0;
constexpr double kStimulusRmsDb = -24.0;

// ---------------------------------------------------------------------------
// Deterministic random numbers (seed 42) and a radix-2 FFT
// ---------------------------------------------------------------------------
struct Rng
{
    std::uint64_t s = 42;
    double uniform() noexcept // (0, 1)
    {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return (double(s >> 11) + 0.5) * (1.0 / 9007199254740992.0);
    }
    double gaussian() noexcept
    {
        return std::sqrt(-2.0 * std::log(uniform())) * std::cos(2.0 * kPi * uniform());
    }
};

template <typename Real>
void fft(std::vector<std::complex<Real>>& a, bool inverse)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double angle = (inverse ? 2.0 : -2.0) * kPi / double(len);
        const std::complex<double> step(std::cos(angle), std::sin(angle));
        for (size_t i = 0; i < n; i += len)
        {
            std::complex<double> w(1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k)
            {
                const std::complex<Real> u = a[i + k];
                const std::complex<Real> v = a[i + k + len / 2] * std::complex<Real>(Real(w.real()), Real(w.imag()));
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= step;
            }
        }
    }
    if (inverse)
        for (auto& x : a)
            x /= Real(n);
}

// ---------------------------------------------------------------------------
// Rooms
// ---------------------------------------------------------------------------
struct RoomSpec
{
    const char* name;
    double rt60;
    int delay;         // D, samples
    double modes[3];   // Hz
    double q;
};

const RoomSpec kRooms[] = {
    { "Room_Small", 0.6, 240, { 420.0, 1850.0, 3900.0 }, 30.0 },
    { "Room_Hall", 1.2, 576, { 280.0, 1250.0, 2600.0 }, 45.0 },
    { "Room_Large", 2.0, 1200, { 190.0, 920.0, 2150.0 }, 60.0 },
};

struct Room
{
    RoomSpec spec;
    std::vector<float> h;      // impulse response after the delay D
    double predictedG0Db = 0;  // from the loop response, plug-in bypassed
    double predictedHz = 0;
};

/** Exponentially decaying Gaussian noise plus three resonant modes. The
    modes stand 14, 12.5 and 11 dB above the RMS level of the diffuse part. */
Room makeRoom(const RoomSpec& spec, Rng& rng)
{
    Room room;
    room.spec = spec;
    const size_t length = size_t(spec.rt60 * kFs); // -60 dB at the end
    std::vector<double> h(length);
    const double decay = std::log(1000.0) / (spec.rt60 * kFs);
    double diffuseEnergy = 0.0;
    for (size_t n = 0; n < length; ++n)
    {
        h[n] = rng.gaussian() * std::exp(-decay * double(n));
        diffuseEnergy += h[n] * h[n];
    }
    const double diffuseRms = std::sqrt(diffuseEnergy); // RMS magnitude of the diffuse spectrum
    const double levelsDb[3] = { 14.0, 12.5, 11.0 };
    for (int i = 0; i < 3; ++i)
    {
        const double f = spec.modes[i];
        const double tau = spec.q / (kPi * f) * kFs; // envelope time constant, samples
        const double amplitude = diffuseRms * std::pow(10.0, levelsDb[i] / 20.0) * 2.0 / tau;
        const double phase = 2.0 * kPi * rng.uniform();
        for (size_t n = 0; n < length; ++n)
            h[n] += amplitude * std::exp(-double(n) / tau) * std::sin(2.0 * kPi * f * double(n) / kFs + phase);
    }

    // Loop response with the delay, on a fine grid. The loop is unstable when
    // G * Re(H) > 1 at a frequency where the phase is a multiple of 2 pi.
    size_t n = 1;
    while (n < 8 * (length + size_t(spec.delay)))
        n <<= 1;
    std::vector<std::complex<double>> spectrum(n);
    for (size_t i = 0; i < length; ++i)
        spectrum[i + size_t(spec.delay)] = h[i];
    fft(spectrum, false);
    double peak = 0.0;
    for (size_t k = 1; k < n / 2; ++k)
        peak = std::max(peak, std::abs(spectrum[k]));
    double bestReal = 0.0;
    size_t bestBin = 0;
    for (size_t k = 1; k + 1 < n / 2; ++k)
    {
        const auto a = spectrum[k], b = spectrum[k + 1];
        if (a.real() > 0.0 && b.real() > 0.0 && (a.imag() < 0.0) != (b.imag() < 0.0))
        {
            const double t = a.imag() / (a.imag() - b.imag());
            const double re = a.real() + t * (b.real() - a.real());
            if (re > bestReal) { bestReal = re; bestBin = k; }
        }
    }
    // Normalise so that the largest magnitude of the room response is 0 dB.
    room.h.resize(length);
    for (size_t i = 0; i < length; ++i)
        room.h[i] = float(h[i] / peak);
    room.predictedG0Db = -dbFromLinear(bestReal / peak);
    room.predictedHz = double(bestBin) * kFs / double(n);
    return room;
}

// ---------------------------------------------------------------------------
// Uniformly partitioned FFT convolver (overlap-save). One block of B input
// samples in, one block of B output samples out, no added latency.
// ---------------------------------------------------------------------------
class PartitionedConvolver
{
public:
    static constexpr int kBlock = 128;

    explicit PartitionedConvolver(const std::vector<float>& h)
    {
        partitions = (h.size() + size_t(kBlock) - 1) / size_t(kBlock);
        response.assign(partitions * kBins, {});
        history.assign(partitions * kBins, {});
        std::vector<std::complex<float>> buf(2 * kBlock);
        for (size_t p = 0; p < partitions; ++p)
        {
            for (auto& x : buf) x = {};
            for (int i = 0; i < kBlock; ++i)
            {
                const size_t idx = p * size_t(kBlock) + size_t(i);
                buf[size_t(i)] = idx < h.size() ? h[idx] : 0.0f;
            }
            fft(buf, false);
            for (size_t k = 0; k < kBins; ++k)
                response[p * kBins + k] = buf[k];
        }
        work.resize(2 * kBlock);
        window.assign(2 * kBlock, 0.0f);
    }

    void reset()
    {
        std::fill(history.begin(), history.end(), std::complex<float>());
        std::fill(window.begin(), window.end(), 0.0f);
        head = 0;
    }

    void process(const float* in, float* out)
    {
        std::memmove(window.data(), window.data() + kBlock, sizeof(float) * kBlock);
        std::memcpy(window.data() + kBlock, in, sizeof(float) * kBlock);
        for (size_t i = 0; i < size_t(2 * kBlock); ++i)
            work[i] = window[i];
        fft(work, false);
        head = (head + partitions - 1) % partitions;
        for (size_t k = 0; k < kBins; ++k)
            history[head * kBins + k] = work[k];

        // Complex multiply-accumulate written out by hand (no NaN handling
        // path of std::complex in the hot loop).
        float accRe[kBins] {}, accIm[kBins] {};
        for (size_t p = 0; p < partitions; ++p)
        {
            const std::complex<float>* x = &history[((head + p) % partitions) * kBins];
            const std::complex<float>* r = &response[p * kBins];
            for (size_t k = 0; k < kBins; ++k)
            {
                accRe[k] += x[k].real() * r[k].real() - x[k].imag() * r[k].imag();
                accIm[k] += x[k].real() * r[k].imag() + x[k].imag() * r[k].real();
            }
        }
        for (size_t k = 0; k < kBins; ++k)
            work[k] = { accRe[k], accIm[k] };
        for (size_t k = 1; k < size_t(kBlock); ++k)
            work[size_t(2 * kBlock) - k] = { accRe[k], -accIm[k] };
        fft(work, true);
        for (int i = 0; i < kBlock; ++i)
            out[i] = work[size_t(kBlock + i)].real();
    }

private:
    static constexpr size_t kBins = kBlock + 1;
    size_t partitions = 0, head = 0;
    std::vector<std::complex<float>> response, history, work;
    std::vector<float> window;
};

void testConvolver()
{
    section("partitioned convolver vs direct convolution");
    Rng rng;
    std::vector<float> h(1000), x(4 * PartitionedConvolver::kBlock * 8), fast(x.size()), direct(x.size());
    for (auto& v : h) v = float(rng.gaussian());
    for (auto& v : x) v = float(rng.gaussian());
    PartitionedConvolver conv(h);
    for (size_t pos = 0; pos < x.size(); pos += size_t(PartitionedConvolver::kBlock))
        conv.process(x.data() + pos, fast.data() + pos);
    double maxErr = 0.0, scale = 0.0;
    for (size_t n = 0; n < x.size(); ++n)
    {
        double acc = 0.0;
        for (size_t k = 0; k < h.size() && k <= n; ++k)
            acc += double(h[k]) * double(x[n - k]);
        direct[n] = float(acc);
        maxErr = std::max(maxErr, std::abs(acc - double(fast[n])));
        scale = std::max(scale, std::abs(acc));
    }
    std::printf("  max error %.3e (signal peak %.1f)\n", maxErr, scale);
    ZFB_CHECK_MSG(maxErr < 1.0e-3 * scale, fmt("relative error %.3e", maxErr / scale));
}

// ---------------------------------------------------------------------------
// Stimulus: synthetic speech-like audio, 2 s on / 1 s off, RMS -24 dBFS while on
// ---------------------------------------------------------------------------
std::vector<float> makeStimulus(Rng& rng)
{
    const size_t total = size_t(kRunSeconds * kFs);
    std::vector<float> s(total, 0.0f);
    double energy = 0.0;
    size_t active = 0;
    double noiseState = 0.0;
    for (size_t n = 0; n < total; ++n)
    {
        const double t = double(n) / kFs;
        if (std::fmod(t, 3.0) >= 2.0)
            continue;
        const double syllable = std::pow(std::max(0.0, std::sin(2.0 * kPi * 3.7 * t)), 2.0);
        const double f0 = 118.0 + 14.0 * std::sin(2.0 * kPi * 0.6 * t);
        double voiced = 0.0;
        for (int hnum = 1; hnum <= 30; ++hnum)
        {
            const double f = f0 * hnum;
            const double formant = std::exp(-std::pow((f - 650.0) / 280.0, 2.0))
                                 + 0.5 * std::exp(-std::pow((f - 1700.0) / 450.0, 2.0))
                                 + 0.2 * std::exp(-std::pow((f - 2900.0) / 600.0, 2.0)) + 0.03;
            voiced += formant * std::sin(2.0 * kPi * f * t + 0.7 * hnum);
        }
        noiseState = 0.6 * noiseState + rng.gaussian(); // mild low-pass on the breath noise
        const double v = syllable * (voiced + 0.15 * noiseState);
        s[n] = float(v);
        energy += v * v;
        ++active;
    }
    const double gain = std::pow(10.0, kStimulusRmsDb / 20.0) / std::sqrt(energy / double(active));
    for (auto& v : s)
        v = float(double(v) * gain);
    return s;
}

// ---------------------------------------------------------------------------
// Estimators used by the bench
// ---------------------------------------------------------------------------

/** Rule-based stand-in for the trained model, for exercising the bench and the
    FSM in a closed loop. It is not part of the product. A band is cut when it
    stands out from its neighbours (ring index > 4 dB), carries little
    modulation (< 6 dB) and is above the long-term overall level.
    The ring threshold is low on purpose: a steady pure tone reaches only
    2.7 to 10.8 dB of ring index depending on where it falls between two band
    centres (SPEC 2.5, open issue). */
struct RuleEstimator : MaskEstimator
{
    void reset() noexcept override {}
    void infer(const float* f, float* masks) noexcept override
    {
        for (int b = 0; b < kNumBands; ++b)
        {
            const float level = f[1 * kNumBands + b];      // (E_s - E_global) / 20
            const float modulation = f[3 * kNumBands + b]; // (M_d - 12) / 12
            const float ring = f[4 * kNumBands + b];       // T_raw / 15
            const bool feedback = ring > 4.0f / 15.0f && modulation < -0.5f && level > 0.0f;
            masks[b] = feedback ? 0.1f : 1.0f;
            masks[kNumBands + b] = 1.0f;
        }
    }
};

enum class Mode { bypass, unityMasks, testWeights, ruleBased };

const char* modeName(Mode m)
{
    switch (m)
    {
        case Mode::bypass:      return "bypass";
        case Mode::unityMasks:  return "active, all masks 1";
        case Mode::testWeights: return "active, random test weights";
        case Mode::ruleBased:   return "active, rule-based stand-in";
    }
    return "";
}

// ---------------------------------------------------------------------------
// One 10 s closed-loop run
// ---------------------------------------------------------------------------
struct RunResult
{
    bool diverged = false;
    double divergedAt = 0.0;   // seconds
    int huntingBursts = 0;
    double hoppingMs = -1.0;   // longest mode excursion, -1 when none occurred
    bool hoppingUnsettled = false;
    double maxGainReductionDb = 0.0;
    int failsafeTrips = 0;
};

RunResult runLoop(const Room& room, const std::vector<float>& stimulus, double gainDb, Mode mode)
{
    constexpr int B = PartitionedConvolver::kBlock;
    const size_t total = stimulus.size() / size_t(B) * size_t(B);
    const float loopGain = float(std::pow(10.0, gainDb / 20.0));
    const size_t delay = size_t(room.spec.delay);

    PartitionedConvolver conv(room.h);
    auto engine = std::make_unique<ZeroFbEngine>();
    auto net = std::make_unique<CausalGruEngine>(embedded::kWeights);
    RuleEstimator rule;
    if (mode == Mode::testWeights) engine->setEstimator(net.get());
    if (mode == Mode::ruleBased) engine->setEstimator(&rule);
    engine->prepare(kFs, 1);
    EngineParams params; // SUPPRESS 75 %, DEREVERB 50 %, FLOOR -24 dB, SPEECH
    params.bypass = mode == Mode::bypass;
    engine->setParams(params);

    // Band powers of the plug-in output, for the hunting and hopping measures.
    auto monitor = std::make_unique<SidechainAnalyzer>();
    monitor->prepare(kFs, kHop);
    float unusedFeatures[kFeatureSize];
    const BandLayout layout;
    int modeBand[3];
    for (int i = 0; i < 3; ++i)
    {
        modeBand[i] = 0;
        for (int b = 1; b < kNumBands; ++b)
            if (std::abs(std::log(layout.fc[size_t(b)] / room.spec.modes[i]))
                < std::abs(std::log(layout.fc[size_t(modeBand[i])] / room.spec.modes[i])))
                modeBand[i] = b;
    }

    std::vector<float> y(total + size_t(B), 0.0f); // plug-in output history
    float delayed[B], feedback[B], mic[B], out[B];

    RunResult result;
    const double stimulusRms = std::pow(10.0, kStimulusRmsDb / 20.0);
    const double pauseLimit = stimulusRms * std::pow(10.0, 6.0 / 20.0);
    const double loudLimit = std::pow(10.0, -3.0 / 20.0);
    const size_t window50 = size_t(0.050 * kFs), hold100 = size_t(0.100 * kFs);
    double energy50 = 0.0;
    size_t loudRun = 0;
    double pauseEnergy = 0.0;
    size_t pauseCount = 0;

    // Per-band state for hunting (50 ms moving power = 150 frames) and hopping.
    constexpr int kAvgFrames = 150;
    std::vector<float> bandHistory(size_t(kAvgFrames) * kNumBands, 0.0f);
    double bandSum[kNumBands] {};
    bool wasSuppressed[kNumBands] {}, burstArmed[kNumBands] {};
    double pauseMinDb[kNumBands];
    bool excursion[3] {};
    double excursionStart[3] {};
    size_t frame = 0;

    for (size_t pos = 0; pos < total && ! result.diverged; pos += size_t(B))
    {
        for (int i = 0; i < B; ++i)
        {
            const size_t n = pos + size_t(i);
            delayed[i] = n >= delay ? y[n - delay] : 0.0f;
        }
        conv.process(delayed, feedback);
        for (int i = 0; i < B; ++i)
            mic[i] = stimulus[pos + size_t(i)] + loopGain * feedback[i];

        const float* in[1] = { mic };
        float* outs[1] = { out };
        // Blocks of one hop, so that band gains can be sampled every frame.
        for (int offset = 0; offset < B; offset += kHop)
        {
            const float* inHop[1] = { in[0] + offset };
            float* outHop[1] = { outs[0] + offset };
            engine->process(inHop, outHop, 1, 1, kHop);

            const double t = double(pos + size_t(offset)) / kFs;
            const bool inPause = std::fmod(t, 3.0) >= 2.0;
            for (int i = 0; i < kHop; ++i)
                monitor->tick(out[offset + i], 0.0f, false);
            monitor->computeFrame(0.0f, unusedFeatures);
            const float* eS = monitor->bandEnergyDb();
            const float* gains = engine->bandGainsDb();
            const size_t slot = frame % size_t(kAvgFrames);
            for (int b = 0; b < kNumBands; ++b)
            {
                const float power = std::pow(10.0f, eS[b] / 10.0f);
                bandSum[b] += double(power) - double(bandHistory[slot * kNumBands + size_t(b)]);
                bandHistory[slot * kNumBands + size_t(b)] = power;
                result.maxGainReductionDb = std::min(result.maxGainReductionDb, double(gains[b]));
                if (gains[b] < -3.0f)
                    wasSuppressed[b] = true;

                // Hunting: inside a pause, a band that had been suppressed
                // rises 10 dB or more above its lowest level of that pause.
                const double avgDb = 10.0 * std::log10(std::max(bandSum[b], 0.0) / kAvgFrames + 1.0e-12);
                if (! inPause)
                {
                    pauseMinDb[b] = 1.0e9;
                    burstArmed[b] = true;
                }
                else if (frame >= size_t(kAvgFrames))
                {
                    pauseMinDb[b] = std::min(pauseMinDb[b], avgDb);
                    if (wasSuppressed[b] && burstArmed[b] && avgDb >= pauseMinDb[b] + 10.0 && avgDb > -60.0)
                    {
                        ++result.huntingBursts;
                        burstArmed[b] = false;
                    }
                    else if (avgDb < pauseMinDb[b] + 3.0)
                    {
                        burstArmed[b] = true;
                    }
                }
            }

            // Mode hopping: after one mode band has been suppressed, time for
            // another mode band from rising above -20 dBFS to falling below -30 dBFS.
            for (int i = 0; i < 3; ++i)
            {
                bool otherSuppressed = false;
                for (int j = 0; j < 3; ++j)
                    otherSuppressed = otherSuppressed || (j != i && modeBand[j] != modeBand[i] && wasSuppressed[modeBand[j]]);
                const float level = eS[modeBand[i]];
                if (! excursion[i] && otherSuppressed && level > -20.0f)
                {
                    excursion[i] = true;
                    excursionStart[i] = t;
                }
                else if (excursion[i] && level < -30.0f)
                {
                    excursion[i] = false;
                    result.hoppingMs = std::max(result.hoppingMs, (t - excursionStart[i]) * 1000.0);
                }
            }
            ++frame;
        }

        for (int i = 0; i < B; ++i)
        {
            const size_t n = pos + size_t(i);
            const float v = out[i];
            y[n] = v;
            const double t = double(n) / kFs;

            // Divergence, rule 1: 50 ms moving RMS above -3 dBFS for 100 ms.
            energy50 += double(v) * double(v);
            if (n >= window50)
                energy50 -= double(y[n - window50]) * double(y[n - window50]);
            const double rms50 = std::sqrt(std::max(energy50, 0.0) / double(window50));
            loudRun = rms50 > loudLimit ? loudRun + 1 : 0;

            // Divergence, rule 2: output RMS over a pause more than 6 dB above
            // the stimulus RMS (evaluated at the end of each pause).
            const double phase = std::fmod(t, 3.0);
            if (phase >= 2.0)
            {
                pauseEnergy += double(v) * double(v);
                ++pauseCount;
            }
            const bool pauseEnds = pauseCount > 0 && (phase < 2.0 || n + 1 == total);
            const bool pauseTooLoud = pauseEnds && std::sqrt(pauseEnergy / double(pauseCount)) > pauseLimit;
            if (pauseEnds)
            {
                pauseEnergy = 0.0;
                pauseCount = 0;
            }

            if (loudRun >= hold100 || pauseTooLoud || ! std::isfinite(v) || std::abs(v) > 64.0f)
            {
                result.diverged = true;
                result.divergedAt = t;
                break;
            }
        }
    }

    for (int i = 0; i < 3; ++i)
        if (excursion[i])
        {
            result.hoppingUnsettled = true;
            result.hoppingMs = std::max(result.hoppingMs, (kRunSeconds - excursionStart[i]) * 1000.0);
        }
    result.failsafeTrips = engine->failsafeTrips();
    return result;
}

/** Highest gain (0.25 dB resolution) that does not diverge, by bisection. */
double findLimitDb(const Room& room, const std::vector<float>& stimulus, Mode mode, double lo, double hi)
{
    while (hi - lo > 0.25 + 1.0e-9)
    {
        const double mid = 0.5 * (lo + hi);
        if (runLoop(room, stimulus, mid, mode).diverged) hi = mid; else lo = mid;
    }
    return lo;
}

/** SPEC 7.1 step 3: from G0 upwards in 0.5 dB steps until the loop diverges. */
double findMaxGainDb(const Room& room, const std::vector<float>& stimulus, Mode mode, double g0, double maxExtraDb)
{
    double best = g0 - 0.5; // below G0 when even G0 itself diverges
    for (double extra = 0.0; extra <= maxExtraDb + 1.0e-9; extra += 0.5)
    {
        if (runLoop(room, stimulus, g0 + extra, mode).diverged)
            break;
        best = g0 + extra;
    }
    return best;
}

} // namespace

int main(int argc, char** argv)
{
    const bool quick = argc > 1 && std::strcmp(argv[1], "--quick") == 0;
    std::printf("ClosedLoopBench: 48 kHz mono, 10 s per run, synthetic speech-like stimulus (RMS %.0f dBFS)\n", kStimulusRmsDb);
    std::printf("No trained model exists yet: GBF, hunting and hopping figures are informational.\n");

    testConvolver();

    Rng rng;
    const std::vector<float> stimulus = makeStimulus(rng);
    const Mode activeModes[] = { Mode::unityMasks, Mode::testWeights, Mode::ruleBased };
    double gbfSum[3] {};
    int rooms = 0;

    for (const RoomSpec& spec : kRooms)
    {
        const Room room = makeRoom(spec, rng);
        ++rooms;
        section(spec.name);
        std::printf("  RT60 %.1f s, D %d samples, modes %.0f / %.0f / %.0f Hz (Q %.0f), response %zu taps\n",
                    spec.rt60, spec.delay, spec.modes[0], spec.modes[1], spec.modes[2], spec.q, room.h.size());

        // SPEC 7.1 step 2: G0 with the plug-in bypassed.
        const double g0 = findLimitDb(room, stimulus, Mode::bypass, room.predictedG0Db - 6.0, room.predictedG0Db + 6.0);
        std::printf("  G0 measured %+.2f dB, predicted from the loop response %+.2f dB (at %.1f Hz)\n",
                    g0, room.predictedG0Db, room.predictedHz);
        // The search can only see growth that reaches the detection level
        // within 10 s, so the measured limit sits at or slightly above the
        // prediction (slowest for the long, low-frequency modes).
        ZFB_CHECK_MSG(g0 >= room.predictedG0Db - 0.5 && g0 <= room.predictedG0Db + 1.5,
                      fmt("G0 measured %.2f dB, predicted %.2f dB", g0, room.predictedG0Db));
        ZFB_CHECK(! runLoop(room, stimulus, g0 - 3.0, Mode::bypass).diverged);
        ZFB_CHECK(runLoop(room, stimulus, g0 + 3.0, Mode::bypass).diverged);

        for (int m = 0; m < 3; ++m)
        {
            const Mode mode = activeModes[m];
            if (quick && mode == Mode::testWeights)
                continue;
            const double gMax = findMaxGainDb(room, stimulus, mode, g0, 20.0);
            const double gbf = gMax - g0;
            gbfSum[m] += gbf;
            const RunResult fixed = runLoop(room, stimulus, g0 + 4.0, mode); // SPEC 7.1 steps 4 and 5
            std::printf("  %-28s GBF %+5.1f dB | at G0+4 dB: %s, hunting bursts %d, hopping %s, max GR %.1f dB, fail-safe trips %d\n",
                        modeName(mode), gbf,
                        fixed.diverged ? "diverged" : "stable",
                        fixed.huntingBursts,
                        fixed.hoppingMs < 0.0 ? "none" : fmt(fixed.hoppingUnsettled ? ">= %.0f ms (not settled)" : "%.0f ms", fixed.hoppingMs).c_str(),
                        fixed.maxGainReductionDb, fixed.failsafeTrips);

            if (mode == Mode::unityMasks)
            {
                // With every mask at 1 the EQ is flat: the plug-in must not
                // move the stability limit by more than the search resolution.
                ZFB_CHECK_MSG(gbf >= -0.5 && gbf <= 0.5, fmt("GBF with all masks at 1: %.2f dB", gbf));
                ZFB_CHECK(fixed.maxGainReductionDb == 0.0);
            }
        }
    }

    section("summary (average over rooms)");
    for (int m = 0; m < 3; ++m)
        if (! (quick && activeModes[m] == Mode::testWeights))
            std::printf("  %-28s GBF %+5.1f dB\n", modeName(activeModes[m]), gbfSum[m] / rooms);
    std::printf("  acceptance target for the trained model (Step 3): GBF >= +6.0 dB, hunting 0, hopping <= 50 ms\n");
    return finish("ClosedLoopBench");
}
