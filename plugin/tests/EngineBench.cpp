// ZERO-FB - EngineBench: CPU load of one audio callback (SPEC 1.2).
//
// 48 kHz / 16 samples / stereo, 10 s warm-up, then 60 s measured. Reports the
// callback time as a share of the block budget (median, P99, max).
//
// The acceptance figures (median <= 10 %, P99 <= 15 %) are defined for an
// Apple M4 class machine in a Release build. On any other machine, including
// shared CI runners, the numbers are informational. This program therefore
// fails only on a functional problem (non-finite output).
//
// Usage: EngineBench [seconds]

#include "TestUtil.h"
#include "../Source/nn/CausalGruEngine.h"
#include "nn/EmbeddedWeights.h" // generated: fixed test weights, not a trained model

#include <algorithm>
#include <chrono>
#include <memory>

#if defined(__x86_64__) || defined(_M_X64)
 #include <immintrin.h>
#endif

using namespace zerofb;

namespace {

/** Flush-to-zero / denormals-are-zero for the calling thread, as
    juce::ScopedNoDenormals does inside processBlock. */
void disableDenormals()
{
#if defined(__x86_64__) || defined(_M_X64)
    _mm_setcsr(_mm_getcsr() | 0x8040u);
#elif defined(__aarch64__) && ! defined(_MSC_VER)
    std::uint64_t fpcr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1ull << 24);
    __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
#endif
}

struct Result { double median, p99, max; };

Result measure(bool withNetwork, double seconds)
{
    constexpr double fs = 48000.0;
    constexpr int block = 16;
    const size_t warmBlocks = size_t(10.0 * fs / block);
    const size_t blocks = size_t(seconds * fs / block);
    const size_t total = (warmBlocks + blocks) * block;

    // Speech-like level changes plus a steady tone, so that envelopes,
    // modulation statistics and the FSM all stay busy.
    std::vector<float> left(total), right(total);
    zerofbtest::Noise noise;
    for (size_t n = 0; n < total; ++n)
    {
        const double t = double(n) / fs;
        const float env = float(std::max(0.0, std::sin(2.0 * kPi * 3.1 * t)));
        left[n] = 0.15f * env * noise.next() + 0.05f * float(std::sin(2.0 * kPi * 2150.0 * t));
        right[n] = 0.12f * env * noise.next() + 0.05f * float(std::sin(2.0 * kPi * 2150.0 * t + 0.4));
    }

    auto net = std::make_unique<CausalGruEngine>(embedded::kWeights);
    auto engine = std::make_unique<ZeroFbEngine>();
    if (withNetwork)
        engine->setEstimator(net.get());
    engine->prepare(fs, 2);
    engine->setParams(EngineParams {});

    std::vector<double> times;
    times.reserve(blocks);
    bool finite = true;
    using clock = std::chrono::steady_clock;
    for (size_t b = 0; b < warmBlocks + blocks; ++b)
    {
        float* io[2] = { left.data() + b * block, right.data() + b * block };
        const float* in[2] = { io[0], io[1] };
        const auto t0 = clock::now();
        engine->process(in, io, 2, 2, block);
        const auto t1 = clock::now();
        if (b >= warmBlocks)
            times.push_back(std::chrono::duration<double>(t1 - t0).count());
        finite = finite && std::isfinite(io[0][0]) && std::isfinite(io[1][block - 1]);
    }
    ZFB_CHECK(finite);
    ZFB_CHECK(engine->failsafeTrips() == 0);

    std::sort(times.begin(), times.end());
    const double budget = double(block) / fs;
    return { 100.0 * times[times.size() / 2] / budget,
             100.0 * times[size_t(double(times.size()) * 0.99)] / budget,
             100.0 * times.back() / budget };
}

} // namespace

int main(int argc, char** argv)
{
    const double seconds = argc > 1 ? std::atof(argv[1]) : 60.0;
    disableDenormals();
#if defined(ZEROFB_SIMD_AVX2)
    const char* simdName = "AVX2";
#elif defined(ZEROFB_SIMD_NEON)
    const char* simdName = "NEON";
#else
    const char* simdName = "scalar";
#endif
    std::printf("EngineBench: 48 kHz / 16 samples / stereo, %s, %.0f s measured after 10 s warm-up\n", simdName, seconds);
    std::printf("block budget = 333.3 us; values are %% of the budget\n");
    const Result dsp = measure(false, seconds);
    const Result full = measure(true, seconds);
    std::printf("  DSP only (no network)      median %6.2f %%   P99 %6.2f %%   max %7.2f %%\n", dsp.median, dsp.p99, dsp.max);
    std::printf("  DSP + neural engine        median %6.2f %%   P99 %6.2f %%   max %7.2f %%\n", full.median, full.p99, full.max);
    std::printf("  target on Apple M4 class:  median <= 10 %%, P99 <= 15 %%  -> %s on this machine\n",
                full.median <= 10.0 && full.p99 <= 15.0 ? "within target" : "above target");
    return zerofbtest::finish("EngineBench");
}
