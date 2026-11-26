// ZERO-FB - GoldenParityTest (SPEC 7.2 item 1).
//
// Compares the C++ core with the independent NumPy reference
// (training/erb_dsp.py) using fixtures from training/gen_golden_fixtures.py.
//
// Usage: GoldenParityTest FIXTURE_DIR

#include "TestUtil.h"
#include "../Source/nn/CausalGruEngine.h"
#include "nn/EmbeddedWeights.h" // generated: fixed test weights, not a trained model

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

// Network outputs with the fixed test weights (layer outputs and masks).
// The SIMD dot products sum in a different order than NumPy, so even the
// exact-math build is compared at a tolerance. Measured: <= 4.1e-6 against
// the float32 reference and <= 2.6e-5 against float64 (SPEC 7.2).
constexpr double kNetToleranceVsFloat32 = 2.0e-5;
constexpr double kNetToleranceVsFloat64 = 1.0e-4;
// Masks computed end to end (C++ features into the C++ network).
constexpr double kMaskToleranceEndToEnd = 1.0e-4;

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

void testVectorActivations()
{
    section("SIMD activations are bit-identical to the scalar fast functions");
    using simd::Vec;
    bool same = true;
    float in[8], outT[8], outS[8];
    for (int i = 0; i <= 400000; ++i)
    {
        const float x = -25.0f + 50.0f * float(i) / 400000.0f;
        for (int k = 0; k < 8; ++k)
            in[k] = x + float(k) * 1.0e-4f;
        for (int k = 0; k < 8; k += Vec::width)
        {
            simd::fastTanh(Vec::load(in + k)).store(outT + k);
            simd::fastSigmoid(Vec::load(in + k)).store(outS + k);
        }
        for (int k = 0; k < 8; ++k)
            same = same && outT[k] == fast_tanh(in[k]) && outS[k] == fast_sigmoid(in[k]);
    }
    ZFB_CHECK(same);
}

/** Loads a .swmodel into float-aligned storage. */
std::vector<float> loadModel(const std::string& path, model::FileStatus& status)
{
    const auto bytes = readFile<unsigned char>(path);
    std::vector<float> storage((bytes.size() + 3) / 4);
    if (! bytes.empty())
        std::memcpy(storage.data(), bytes.data(), bytes.size());
    status = model::checkFile(storage.data(), bytes.size());
    return storage;
}

void testModelFile(const std::string& dir)
{
    section(".swmodel container (671,616 bytes, CRC32) and embedded weights");
    model::FileStatus status;
    auto file = loadModel(dir + "/test_model.swmodel", status);
    ZFB_CHECK(file.size() * 4 == model::kFileBytes);
    ZFB_CHECK(status == model::FileStatus::ok);
    if (status != model::FileStatus::ok)
        return;

    auto* bytes = reinterpret_cast<unsigned char*>(file.data());
    ZFB_CHECK(model::crc32(reinterpret_cast<const unsigned char*>("123456789"), 9) == 0xCBF43926u); // standard check value
    ZFB_CHECK(model::checkFile(bytes, model::kFileBytes - 1) == model::FileStatus::wrongSize);
    bytes[model::kHeaderBytes + 1000] ^= 0x01;
    ZFB_CHECK(model::checkFile(bytes, model::kFileBytes) == model::FileStatus::wrongCrc);
    bytes[model::kHeaderBytes + 1000] ^= 0x01;
    bytes[0] = 'X';
    ZFB_CHECK(model::checkFile(bytes, model::kFileBytes) == model::FileStatus::wrongMagic);
    bytes[0] = 'S';
    bytes[0x18] ^= 0x01; // hidden_size
    ZFB_CHECK(model::checkFile(bytes, model::kFileBytes) == model::FileStatus::wrongHeader);
    bytes[0x18] ^= 0x01;
    ZFB_CHECK(model::checkFile(bytes, model::kFileBytes) == model::FileStatus::ok);

    // The generated header carries exactly the payload of the file.
    ZFB_CHECK(embedded::kWeightCount == model::kTotalParams);
    ZFB_CHECK(embedded::kPayloadCrc32 == model::readU32(bytes + 0x20));
    ZFB_CHECK(std::memcmp(embedded::kWeights, bytes + model::kHeaderBytes, size_t(model::kTotalParams) * 4) == 0);
    ZFB_CHECK(reinterpret_cast<std::uintptr_t>(embedded::kWeights) % 64 == 0);
}

struct LayerErrors { double e32 = 0.0, e64 = 0.0; };

void testNetworkLayers(const std::string& dir)
{
    section("network layers vs NumPy reference (reference features as input)");
    const auto feat = readFile<float>(dir + "/features_48000_stereo.f32");
    const auto ref32 = readFile<float>(dir + "/nn_layers_48000_stereo.f32");
    const auto ref64 = readFile<double>(dir + "/nn_layers_48000_stereo.f64");
    constexpr int kLayerTotal = model::kConvOut + 3 * model::kHidden + kMaskSize; // 864
    const size_t frames = ref32.size() / size_t(kLayerTotal);
    ZFB_CHECK(frames >= 1000 && ref64.size() == ref32.size() && feat.size() >= frames * size_t(kFeatureSize));
    if (frames == 0 || ref64.size() != ref32.size())
        return;

    auto net = std::make_unique<CausalGruEngine>(embedded::kWeights);
    const char* names[5] = { "conv1", "proj", "gru1", "gru2", "masks" };
    const int sizes[5] = { model::kConvOut, model::kHidden, model::kHidden, model::kHidden, kMaskSize };
    LayerErrors err[5];
    float masks[kMaskSize];
    for (size_t k = 0; k < frames; ++k)
    {
        net->infer(feat.data() + k * size_t(kFeatureSize), masks);
        const float* got[5] = { net->convOutput(), net->projOutput(), net->hidden1(), net->hidden2(), masks };
        size_t offset = k * size_t(kLayerTotal);
        for (int layer = 0; layer < 5; ++layer)
        {
            for (int i = 0; i < sizes[layer]; ++i)
            {
                err[layer].e32 = std::max(err[layer].e32, std::abs(double(got[layer][i]) - double(ref32[offset + size_t(i)])));
                err[layer].e64 = std::max(err[layer].e64, std::abs(double(got[layer][i]) - ref64[offset + size_t(i)]));
            }
            offset += size_t(sizes[layer]);
        }
    }
    for (int layer = 0; layer < 5; ++layer)
    {
        std::printf("  %-6s vs float32 %.3e   vs float64 %.3e\n", names[layer], err[layer].e32, err[layer].e64);
        ZFB_CHECK_MSG(err[layer].e32 < kNetToleranceVsFloat32, fmt("%.3e (float32 reference)", err[layer].e32));
        ZFB_CHECK_MSG(err[layer].e64 < kNetToleranceVsFloat64, fmt("%.3e (float64 reference)", err[layer].e64));
    }

    // reset() returns the network to its initial state.
    net->reset();
    net->infer(feat.data(), masks);
    double eReset = 0.0;
    for (int i = 0; i < kMaskSize; ++i)
        eReset = std::max(eReset, std::abs(double(masks[i]) - double(ref32[size_t(kLayerTotal - kMaskSize + i)])));
    ZFB_CHECK_MSG(eReset < kNetToleranceVsFloat32, fmt("after reset %.3e", eReset));

    // Without weights the estimator passes everything.
    CausalGruEngine empty;
    empty.infer(feat.data(), masks);
    bool unity = true;
    for (int i = 0; i < kMaskSize; ++i)
        unity = unity && masks[i] == 1.0f;
    ZFB_CHECK(unity && ! empty.hasWeights());
}

/** Forwards to the real network and keeps a copy of the masks of each frame. */
struct CapturingEstimator : MaskEstimator
{
    CausalGruEngine net { embedded::kWeights };
    std::vector<float> masks;
    size_t frame = 0;
    void reset() noexcept override { net.reset(); }
    void infer(const float* features, float* out) noexcept override
    {
        net.infer(features, out);
        if ((frame + 1) * size_t(kMaskSize) <= masks.size())
            std::memcpy(masks.data() + frame * size_t(kMaskSize), out, sizeof(float) * size_t(kMaskSize));
        ++frame;
    }
};

void testMasksEndToEnd(const std::string& dir)
{
    section("64-channel masks end to end (audio -> C++ features -> C++ network)");
    for (const Case& c : kCases)
    {
        const auto signal = readFile<float>(dir + "/signal_" + c.name + ".f32");
        const auto ref64 = readFile<double>(dir + "/masks_" + c.name + ".f64");
        const auto ref32 = readFile<float>(dir + "/masks_" + c.name + ".f32");
        const int hop = hopForSampleRate(double(c.fs));
        const size_t samples = signal.size() / size_t(c.channels);
        const size_t frames = samples / size_t(hop);
        ZFB_CHECK(frames > 0 && ref64.size() == frames * size_t(kMaskSize) && ref32.size() == ref64.size());
        if (frames == 0 || ref64.size() != frames * size_t(kMaskSize) || ref32.size() != ref64.size())
            continue;

        auto estimator = std::make_unique<CapturingEstimator>();
        estimator->masks.assign(ref64.size(), 0.0f);
        auto engine = std::make_unique<ZeroFbEngine>();
        engine->setEstimator(estimator.get());
        engine->prepare(double(c.fs), c.channels);
        engine->setParams(EngineParams {});

        std::vector<float> chan[2], out[2];
        for (int ch = 0; ch < 2; ++ch) { chan[ch].resize(samples); out[ch].resize(samples); }
        for (size_t n = 0; n < samples; ++n)
            for (int ch = 0; ch < c.channels; ++ch)
                chan[ch][n] = signal[n * size_t(c.channels) + size_t(ch)];
        for (size_t pos = 0; pos < frames * size_t(hop);)
        {
            const int n = int(std::min<size_t>(113, frames * size_t(hop) - pos));
            const float* in[2] = { chan[0].data() + pos, chan[1].data() + pos };
            float* o[2] = { out[0].data() + pos, out[1].data() + pos };
            engine->process(in, o, c.channels, c.channels, n);
            pos += size_t(n);
        }
        ZFB_CHECK(estimator->frame == frames);

        double e32 = 0.0, e64 = 0.0;
        float lo = 1.0f, hi = 0.0f;
        for (size_t i = 0; i < ref64.size(); ++i)
        {
            e32 = std::max(e32, std::abs(double(estimator->masks[i]) - double(ref32[i])));
            e64 = std::max(e64, std::abs(double(estimator->masks[i]) - ref64[i]));
            lo = std::min(lo, estimator->masks[i]);
            hi = std::max(hi, estimator->masks[i]);
        }
        std::printf("  %-13s vs float32 %.3e   vs float64 %.3e   (mask range %.3f .. %.3f)\n", c.name, e32, e64, lo, hi);
        ZFB_CHECK_MSG(e32 < kMaskToleranceEndToEnd, fmt("%.3e (float32 reference)", e32));
        ZFB_CHECK_MSG(e64 < kMaskToleranceEndToEnd, fmt("%.3e (float64 reference)", e64));
        ZFB_CHECK(hi - lo > 0.2f); // the test weights must actually move the masks
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
    testVectorActivations();
    testModelFile(argv[1]);
    testNetworkLayers(argv[1]);
    testMasksEndToEnd(argv[1]);
    return finish("GoldenParityTest");
}
