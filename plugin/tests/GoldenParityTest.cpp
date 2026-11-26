// ZERO-FB - GoldenParityTest (SPEC 7.2 item 1).
//
// Compares the C++ core with the independent NumPy reference
// (training/erb_dsp.py) using fixtures from training/gen_golden_fixtures.py.
//
// Usage: GoldenParityTest FIXTURE_DIR

#include "TestUtil.h"

#include <cstring>
#include <fstream>
#include <memory>

using namespace zerofb;
using namespace zerofbtest;

namespace {

// Tolerances (maximum absolute error, feature units after scaling).
//   vs float32 reference: same arithmetic, so only the transcendental
//   functions may differ (log10 of different C libraries can differ by a unit
//   in the last place, which is 7.6e-6 dB near -100 dB). Tight for the
//   exact-math build, wider for fast math.
//   vs float64 reference: bounded by single precision rounding noise.
#if defined(ZEROFB_EXACT_MATH)
constexpr double kToleranceVsFloat32 = 2.0e-5;
#else
constexpr double kToleranceVsFloat32 = 1.0e-4;
#endif
constexpr double kToleranceVsFloat64 = 1.0e-3;
constexpr double kFastMathTolerance = 1.0e-5;

template <typename T>
std::vector<T> readFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    std::vector<T> data;
    if (! f)
    {
        ZFB_CHECK_MSG(false, "cannot open " + path);
        return data;
    }
    const auto bytes = std::streamsize(f.tellg());
    f.seekg(0);
    data.resize(size_t(bytes) / sizeof(T));
    f.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size() * sizeof(T)));
    return data;
}

void testFastMath()
{
    section("fast math vs std (max abs error < 1e-5)");
    double eLog = 0.0, eExp = 0.0, eTanh = 0.0, eSig = 0.0;
    const int steps = 2000000;
    for (int i = 0; i <= steps; ++i)
    {
        const double u = double(i) / double(steps);

        const double xl = std::pow(10.0, -10.0 + 13.0 * u); // [1e-10, 1e3], log spaced
        eLog = std::max(eLog, std::abs(double(fast_log10(float(xl))) - std::log10(double(float(xl)))));

        const float xe = float(-30.0 + 34.0 * u); // [-30, 4]
        eExp = std::max(eExp, std::abs(double(fast_exp2(xe)) - std::exp2(double(xe))));

        const float xt = float(-25.0 + 50.0 * u); // [-25, 25]
        eTanh = std::max(eTanh, std::abs(double(fast_tanh(xt)) - std::tanh(double(xt))));
        eSig = std::max(eSig, std::abs(double(fast_sigmoid(xt)) - 1.0 / (1.0 + std::exp(-double(xt)))));
    }
    std::printf("  fast_log10 %.3e  fast_exp2 %.3e  fast_tanh %.3e  fast_sigmoid %.3e\n", eLog, eExp, eTanh, eSig);
    ZFB_CHECK_MSG(eLog < kFastMathTolerance, fmt("fast_log10 %.3e", eLog));
    ZFB_CHECK_MSG(eExp < kFastMathTolerance, fmt("fast_exp2 %.3e", eExp));
    ZFB_CHECK_MSG(eTanh < kFastMathTolerance, fmt("fast_tanh %.3e", eTanh));
    ZFB_CHECK_MSG(eSig < kFastMathTolerance, fmt("fast_sigmoid %.3e", eSig));

    // Exact values the pipeline relies on.
    ZFB_CHECK(fast_exp2(0.0f) == 1.0f);
    ZFB_CHECK(fast_log10(1.0f) == 0.0f);
    ZFB_CHECK(fast_tanh(0.0f) == 0.0f);
    ZFB_CHECK(std::abs(fast_tanh(1000.0f)) <= 1.0f && std::abs(fast_tanh(-1000.0f)) <= 1.0f);
}

void testBandLayout()
{
    section("band layout (SPEC 2.2 representative values)");
    const BandLayout l;
    ZFB_CHECK(l.fc[0] == 60.0 && l.fc[31] == 15000.0);
    ZFB_CHECK_MSG(std::abs(l.fc[1] - 99.41) < 0.005, fmt("f1 %.4f", l.fc[1]));
    ZFB_CHECK_MSG(std::abs(l.fc[15] - 1738.52) < 0.005, fmt("f15 %.4f", l.fc[15]));
    ZFB_CHECK_MSG(std::abs(l.fc[30] - 13171.56) < 0.005, fmt("f30 %.4f", l.fc[30]));
    double qMin = 1e9, qMax = 0.0;
    for (double q : l.qSc) { qMin = std::min(qMin, q); qMax = std::max(qMax, q); }
    ZFB_CHECK_MSG(std::abs(qMin - 1.960) < 0.001 && std::abs(qMax - 7.687) < 0.001, fmt("Q range %.4f .. %.4f", qMin, qMax));
    ZFB_CHECK(hopForSampleRate(44100.0) == 15 && hopForSampleRate(48000.0) == 16 && hopForSampleRate(96000.0) == 32);
    ZFB_CHECK(hopForSampleRate(88200.0) == 0);
}

struct Case { int fs; int channels; const char* name; };
const Case kCases[] = {
    { 44100, 2, "44100_stereo" },
    { 48000, 2, "48000_stereo" },
    { 96000, 2, "96000_stereo" },
    { 48000, 1, "48000_mono" },
};

void compareFeatures(const Case& c, const char* path, const std::vector<float>& got,
                     const std::vector<double>& ref64, const std::vector<float>& ref32)
{
    double e64 = 0.0, e32 = 0.0;
    double perChannel64[kNumFeatures] {};
    for (size_t i = 0; i < got.size(); ++i)
    {
        const double d64 = std::abs(double(got[i]) - ref64[i]);
        e64 = std::max(e64, d64);
        e32 = std::max(e32, std::abs(double(got[i]) - double(ref32[i])));
        const size_t ch = (i % size_t(kFeatureSize)) / size_t(kNumBands);
        perChannel64[ch] = std::max(perChannel64[ch], d64);
    }
    std::printf("  %-13s %-8s vs float32 %.3e   vs float64 %.3e  (per channel %.1e %.1e %.1e %.1e %.1e)\n",
                c.name, path, e32, e64, perChannel64[0], perChannel64[1], perChannel64[2], perChannel64[3], perChannel64[4]);
    ZFB_CHECK_MSG(e32 < kToleranceVsFloat32, fmt("%.3e >= %.1e (float32 reference)", e32, kToleranceVsFloat32));
    ZFB_CHECK_MSG(e64 < kToleranceVsFloat64, fmt("%.3e >= %.1e (float64 reference)", e64, kToleranceVsFloat64));
}

void testFeatures(const std::string& dir)
{
    section("6-channel features vs NumPy reference");
    for (const Case& c : kCases)
    {
        const auto signal = readFile<float>(dir + "/signal_" + c.name + ".f32");
        const auto ref64 = readFile<double>(dir + "/features_" + c.name + ".f64");
        const auto ref32 = readFile<float>(dir + "/features_" + c.name + ".f32");
        const int hop = hopForSampleRate(double(c.fs));
        const size_t samples = signal.size() / size_t(c.channels);
        const size_t frames = samples / size_t(hop);
        ZFB_CHECK_MSG(frames > 0 && ref64.size() == frames * size_t(kFeatureSize) && ref32.size() == ref64.size(),
                      std::string("fixture size mismatch for ") + c.name);
        if (frames == 0 || ref64.size() != frames * size_t(kFeatureSize) || ref32.size() != ref64.size())
            continue;

        // 1. The analyzer on its own.
        {
            auto analyzer = std::make_unique<SidechainAnalyzer>();
            analyzer->prepare(double(c.fs), hop);
            std::vector<float> got(ref64.size());
            for (size_t n = 0; n < frames * size_t(hop); ++n)
            {
                const float l = signal[n * size_t(c.channels)];
                const float r = c.channels == 2 ? signal[n * 2 + 1] : 0.0f;
                analyzer->tick(l, r, c.channels == 2);
                if ((n + 1) % size_t(hop) == 0)
                    analyzer->computeFrame(0.0f, got.data() + (n + 1) / size_t(hop) * size_t(kFeatureSize) - size_t(kFeatureSize));
            }
            compareFeatures(c, "analyzer", got, ref64, ref32);
        }

        // 2. Through the engine: checks the hop schedule n_k = (k + 1) H - 1,
        //    with irregular block sizes, and that the control path keeps
        //    running while the plug-in is bypassed.
        for (int bypassed = 0; bypassed < 2; ++bypassed)
        {
            auto engine = std::make_unique<ZeroFbEngine>();
            engine->prepare(double(c.fs), c.channels);
            EngineParams p;
            p.bypass = bypassed == 1;
            engine->setParams(p);
            std::vector<float> got(ref64.size());
            std::vector<float> chan[2], out[2];
            for (int ch = 0; ch < 2; ++ch) { chan[ch].resize(samples); out[ch].resize(samples); }
            for (size_t n = 0; n < samples; ++n)
                for (int ch = 0; ch < c.channels; ++ch)
                    chan[ch][n] = signal[n * size_t(c.channels) + size_t(ch)];

            // Process hop by hop so the features of every frame can be read,
            // splitting each hop into two uneven blocks.
            for (size_t k = 0; k < frames; ++k)
            {
                const size_t pos = k * size_t(hop);
                const int first = 1 + int(k % size_t(hop - 1));
                const float* in1[2] = { chan[0].data() + pos, chan[1].data() + pos };
                float* out1[2] = { out[0].data() + pos, out[1].data() + pos };
                engine->process(in1, out1, c.channels, c.channels, first);
                const float* in2[2] = { chan[0].data() + pos + first, chan[1].data() + pos + first };
                float* out2[2] = { out[0].data() + pos + first, out[1].data() + pos + first };
                engine->process(in2, out2, c.channels, c.channels, hop - first);
                std::memcpy(got.data() + k * size_t(kFeatureSize), engine->lastFeatures(), size_t(kFeatureSize) * sizeof(float));
            }
            compareFeatures(c, bypassed ? "bypassed" : "engine", got, ref64, ref32);
        }
    }
}

void testCoupling(const std::string& dir)
{
    section("coupling matrix and Projected Gauss-Seidel vs NumPy reference");
    for (int fs : { 44100, 48000, 96000 })
    {
        const auto data = readFile<double>(dir + "/coupling_" + std::to_string(fs) + ".f64");
        const size_t matrix = size_t(kNumBands) * size_t(kNumBands);
        const size_t record = size_t(2 * kNumBands + 1);
        ZFB_CHECK(data.size() > matrix && (data.size() - matrix) % record == 0);
        if (data.size() <= matrix)
            continue;

        auto eq = std::make_unique<SerialPeakingEq>();
        eq->prepare(double(fs), hopForSampleRate(double(fs)));
        double eMatrix = 0.0;
        for (int b = 0; b < kNumBands; ++b)
            for (int j = 0; j < kNumBands; ++j)
                eMatrix = std::max(eMatrix, std::abs(double(eq->couplingAt(b, j)) - data[size_t(b) * kNumBands + size_t(j)]));

        double eSolve = 0.0;
        const size_t cases = (data.size() - matrix) / record;
        for (size_t i = 0; i < cases; ++i)
        {
            const double* rec = data.data() + matrix + i * record;
            float target[kNumBands], solved[kNumBands];
            for (int b = 0; b < kNumBands; ++b)
                target[b] = float(rec[b]);
            eq->solveCoupling(target, float(rec[kNumBands]), solved);
            for (int b = 0; b < kNumBands; ++b)
                eSolve = std::max(eSolve, std::abs(double(solved[b]) - rec[kNumBands + 1 + b]));
        }
        std::printf("  fs=%d  matrix %.3e   solve (%zu cases, dB) %.3e\n", fs, eMatrix, cases, eSolve);
        ZFB_CHECK_MSG(eMatrix < 1.0e-6, fmt("coupling matrix %.3e", eMatrix));
        ZFB_CHECK_MSG(eSolve < 1.0e-3, fmt("PGS %.3e dB", eSolve));
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::printf("usage: GoldenParityTest FIXTURE_DIR\n");
        return 2;
    }
#if defined(ZEROFB_EXACT_MATH)
    std::printf("math: std (exact)\n");
#else
    std::printf("math: fast approximations\n");
#endif
#if defined(ZEROFB_SIMD_AVX2)
    std::printf("simd: AVX2\n");
#elif defined(ZEROFB_SIMD_NEON)
    std::printf("simd: NEON\n");
#else
    std::printf("simd: scalar\n");
#endif
    testFastMath();
    testBandLayout();
    testFeatures(argv[1]);
    testCoupling(argv[1]);
    return finish("GoldenParityTest");
}
