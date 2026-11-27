// ZERO-FB - plug-in level checks without a host (SPEC chapter 9, Step 4).
//
//   PluginSoakTest [seconds of audio for the endurance run, default 3600]
//
// Checks the AudioProcessor wrapper: bus layouts, bit-perfect bypass, state
// save / restore, presets, and a long run at 48 kHz / 16 samples that must
// not allocate and must stay finite. Callback time against the 16-sample
// deadline is printed for reference only: a shared CI machine says nothing
// about xruns on a real audio interface.

#include "TestUtil.h"

#include "../Source/PluginProcessor.h"
#include "nn/EmbeddedWeights.h" // fixed test weights, generated at build time

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <new>

// Counts operator new on the thread that is inside processBlock only: JUCE
// runs helper threads of its own that are free to allocate.
static std::atomic<long> gAllocations { 0 };
static thread_local bool tInCallback = false;

void* operator new(std::size_t size)
{
    if (tInCallback)
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

void setParam(ZeroFbAudioProcessor& p, const char* id, float plain)
{
    auto* param = p.apvts.getParameter(id);
    ZFB_CHECK(param != nullptr);
    if (param != nullptr)
        param->setValueNotifyingHost(param->convertTo0to1(plain));
}

float getParam(ZeroFbAudioProcessor& p, const char* id) { return p.apvts.getRawParameterValue(id)->load(); }

bool setLayout(ZeroFbAudioProcessor& p, int numIn, int numOut)
{
    juce::AudioProcessor::BusesLayout layout;
    layout.inputBuses.add(juce::AudioChannelSet::canonicalChannelSet(numIn));
    layout.outputBuses.add(juce::AudioChannelSet::canonicalChannelSet(numOut));
    return p.setBusesLayout(layout);
}

void fillSpeechLike(juce::AudioBuffer<float>& buffer, Noise& noise, std::uint64_t& n)
{
    for (int i = 0; i < buffer.getNumSamples(); ++i, ++n)
    {
        // Noise with a slow level swing so the feature levels keep moving.
        const float env = 0.15f + 0.1f * std::sin(float(double(n) * 2.0 * 3.141592653589793 * 3.0 / 48000.0));
        for (int c = 0; c < buffer.getNumChannels(); ++c)
            buffer.setSample(c, i, env * noise.next());
    }
}

void testLayouts()
{
    section("bus layouts");
    const int layouts[][2] = { { 1, 1 }, { 2, 2 }, { 1, 2 } };
    for (const auto& io : layouts)
    {
        for (double rate : { 44100.0, 48000.0, 96000.0 })
        {
            ZeroFbAudioProcessor p;
            p.useWeights(embedded::kWeights);
            ZFB_CHECK(setLayout(p, io[0], io[1]));
            p.setRateAndBufferSizeDetails(rate, 64);
            p.prepareToPlay(rate, 64);
            ZFB_CHECK(p.getLatencySamples() == 0);
            juce::AudioBuffer<float> buffer(std::max(io[0], io[1]), 64);
            juce::MidiBuffer midi;
            Noise noise;
            std::uint64_t n = 0;
            bool finite = true;
            for (int block = 0; block < 400; ++block)
            {
                fillSpeechLike(buffer, noise, n);
                p.processBlock(buffer, midi);
                for (int c = 0; c < io[1]; ++c)
                    for (int i = 0; i < 64; ++i)
                        finite = finite && std::isfinite(buffer.getSample(c, i));
            }
            ZFB_CHECK_MSG(finite, fmt("in %.0f out %.0f at %.0f Hz", io[0], io[1], rate));
        }
    }
    ZeroFbAudioProcessor p;
    ZFB_CHECK(! setLayout(p, 2, 1));
}

void testBypassBitPerfect()
{
    section("bypass is bit-perfect through the wrapper");
    ZeroFbAudioProcessor p;
    ZFB_CHECK(setLayout(p, 2, 2));
    p.setRateAndBufferSizeDetails(48000.0, 32);
    p.prepareToPlay(48000.0, 32);
    ZFB_CHECK(p.getBypassParameter() != nullptr);
    setParam(p, "bypass", 1.0f);
    juce::AudioBuffer<float> buffer(2, 32), copy(2, 32);
    juce::MidiBuffer midi;
    Noise noise;
    std::uint64_t n = 0;
    long mismatches = 0;
    for (int block = 0; block < 600; ++block)
    {
        fillSpeechLike(buffer, noise, n);
        copy.makeCopyOf(buffer);
        p.processBlock(buffer, midi);
        if (block >= 100) // well past the 5 ms crossfade
            for (int c = 0; c < 2; ++c)
                if (std::memcmp(buffer.getReadPointer(c), copy.getReadPointer(c), sizeof(float) * 32) != 0)
                    ++mismatches;
    }
    ZFB_CHECK_MSG(mismatches == 0, fmt("%.0f blocks differ", double(mismatches)));
}

void testStateAndPresets()
{
    section("state round trip and presets");
    ZeroFbAudioProcessor a;
    ZFB_CHECK(a.getNumPrograms() == 4);
    ZFB_CHECK(a.matchingPreset() == -1); // the defaults (SPEC 6.1) are not one of the presets
    for (int i = 0; i < 4; ++i)
    {
        a.setCurrentProgram(i);
        ZFB_CHECK(a.matchingPreset() == i);
        ZFB_CHECK(a.getCurrentProgram() == i);
        ZFB_CHECK(std::abs(getParam(a, "suppress") - kPresets[size_t(i)].suppress) < 0.05f);
        ZFB_CHECK(std::abs(getParam(a, "dereverb") - kPresets[size_t(i)].dereverb) < 0.05f);
        ZFB_CHECK(std::abs(getParam(a, "floor") - kPresets[size_t(i)].floorDb) < 0.05f);
        ZFB_CHECK((getParam(a, "mode") >= 0.5f) == kPresets[size_t(i)].vocal);
    }

    setParam(a, "suppress", 33.3f);
    setParam(a, "dereverb", 12.5f);
    setParam(a, "floor", -19.0f);
    setParam(a, "mode", 1.0f);
    setParam(a, "bypass", 1.0f);
    setParam(a, "reset", 1.0f);
    juce::MemoryBlock block;
    a.getStateInformation(block);
    const auto xml = juce::AudioProcessor::getXmlFromBinary(block.getData(), int(block.getSize()));
    ZFB_CHECK(xml != nullptr && xml->getIntAttribute("state_version") == ZeroFbAudioProcessor::kStateVersion);

    ZeroFbAudioProcessor b;
    b.setStateInformation(block.getData(), int(block.getSize()));
    ZFB_CHECK(std::abs(getParam(b, "suppress") - 33.3f) < 0.05f);
    ZFB_CHECK(std::abs(getParam(b, "dereverb") - 12.5f) < 0.05f);
    ZFB_CHECK(std::abs(getParam(b, "floor") + 19.0f) < 0.05f);
    ZFB_CHECK(getParam(b, "mode") >= 0.5f);
    ZFB_CHECK(getParam(b, "bypass") >= 0.5f);
    ZFB_CHECK(getParam(b, "reset") < 0.5f); // a stored RESET must not fire on load

    // Garbage must be ignored without touching the parameters.
    const char junk[] = "not a state";
    b.setStateInformation(junk, int(sizeof junk));
    ZFB_CHECK(std::abs(getParam(b, "suppress") - 33.3f) < 0.05f);
}

void testEndurance(double seconds)
{
    section("endurance: 48 kHz / 16 samples / stereo");
    ZeroFbAudioProcessor p;
    p.useWeights(embedded::kWeights);
    ZFB_CHECK(p.isModelActive());
    ZFB_CHECK(setLayout(p, 2, 2));
    p.setRateAndBufferSizeDetails(48000.0, 16);
    p.prepareToPlay(48000.0, 16);
    juce::AudioBuffer<float> buffer(2, 16);
    juce::MidiBuffer midi;
    Noise noise;
    std::uint64_t n = 0;
    const long blocks = long(seconds * 48000.0 / 16.0);
    const double deadline = 16.0 / 48000.0;
    long allocations = 0, nonFinite = 0, late = 0;
    double worst = 0.0, total = 0.0;

    for (long block = 0; block < blocks; ++block)
    {
        fillSpeechLike(buffer, noise, n);
        if (block % 48000 == 24000) // parameter moves and a RESET now and then
        {
            setParam(p, "suppress", float(50 + (block / 48000) % 50));
            setParam(p, "reset", (block / 48000) % 7 == 3 ? 1.0f : 0.0f);
        }
        const long before = gAllocations.load(std::memory_order_relaxed);
        const auto t0 = std::chrono::steady_clock::now();
        tInCallback = true;
        p.processBlock(buffer, midi);
        tInCallback = false;
        const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        allocations += gAllocations.load(std::memory_order_relaxed) - before;
        total += dt;
        worst = std::max(worst, dt);
        if (dt > deadline)
            ++late;
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 16; ++i)
                if (! std::isfinite(buffer.getSample(c, i)))
                    ++nonFinite;
    }

    std::printf("  model: fixed test weights (the network runs every hop)\n");
    std::printf("  %.0f s of audio, %ld callbacks\n", seconds, blocks);
    std::printf("  heap allocations inside processBlock: %ld\n", allocations);
    std::printf("  non-finite output samples: %ld\n", nonFinite);
    std::printf("  [reference] mean load %.2f %%, worst callback %.3f ms, callbacks over the %.3f ms deadline: %ld\n",
                100.0 * total / (double(blocks) * deadline), worst * 1e3, deadline * 1e3, late);
    ZFB_CHECK(allocations == 0);
    ZFB_CHECK(nonFinite == 0);
}

} // namespace

int main(int argc, char** argv)
{
    const double seconds = argc > 1 ? std::atof(argv[1]) : 3600.0;
    juce::ScopedJuceInitialiser_GUI gui;
    testLayouts();
    testBypassBitPerfect();
    testStateAndPresets();
    testEndurance(seconds);
    return finish("PluginSoakTest");
}
