// ZERO-FB - plug-in processor (SPEC chapters 2, 5 and 6).
#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "dsp/ZeroFbEngine.h"
#include "nn/CausalGruEngine.h"

#include <array>
#include <atomic>

namespace zerofb {

/** One factory preset (SPEC 6.2). */
struct Preset
{
    const char* name;      // host program name
    const char* chipLabel; // short label on the preset chip
    float suppress;        // %
    float dereverb;        // %
    float floorDb;
    bool vocal;
};

inline constexpr std::array<Preset, 4> kPresets { {
    { "01_Conference_Lavalier", "CONF LAV", 80.0f, 60.0f, -24.0f, false },
    { "02_Handheld_Speech", "HANDHELD", 75.0f, 40.0f, -24.0f, false },
    { "03_Live_Vocal", "LIVE VOCAL", 65.0f, 25.0f, -15.0f, true },
    { "04_Feedback_Only", "FB ONLY", 85.0f, 0.0f, -30.0f, false },
} };

/** Values published by the audio thread for the editor. One direction only,
    no locks: every member is a std::atomic. */
struct Telemetry
{
    std::array<std::atomic<float>, kNumBands> bandGainDb {};
    std::atomic<std::uint32_t> holdMask { 0 }; // bit b set: band b is holding a feedback cut
    std::atomic<float> inputPeakDb { -100.0f };
    std::atomic<float> outputPeakDb { -100.0f };
    std::atomic<float> cpuLoadPercent { 0.0f };
    std::atomic<int> blockSize { 0 };
    std::atomic<double> sampleRate { 48000.0 };
    std::atomic<bool> sampleRateSupported { true };
};

class ZeroFbAudioProcessor final : public juce::AudioProcessor
{
public:
    ZeroFbAudioProcessor();
    ~ZeroFbAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "ZERO-FB"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    juce::AudioProcessorParameter* getBypassParameter() const override { return bypassParam; }

    int getNumPrograms() override { return int (kPresets.size()); }
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    /** Applies a factory preset to the four tone parameters. */
    void applyPreset (int index);
    /** Index of the preset the current parameter values match, or -1. */
    int matchingPreset() const;

    /** Points the network at a weight payload (SPEC 3.2 layout) that outlives
        the processor. Call before prepareToPlay. The tests use this to run
        the network with the fixed test weights. */
    void useWeights (const float* payload) noexcept;
    /** True when a weight payload is in use, compiled in or set by useWeights.
        Without one all masks stay at 1: audio passes without suppression. */
    bool isModelActive() const noexcept { return modelActive; }

    juce::AudioProcessorValueTreeState apvts;
    Telemetry telemetry;

    static constexpr int kStateVersion = 1;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    ZeroFbEngine engine;
    CausalGruEngine network;

    std::atomic<float>* suppressValue = nullptr;
    std::atomic<float>* dereverbValue = nullptr;
    std::atomic<float>* floorValue = nullptr;
    std::atomic<float>* modeValue = nullptr;
    std::atomic<float>* resetValue = nullptr;
    std::atomic<float>* bypassValue = nullptr;
    juce::AudioParameterBool* bypassParam = nullptr;

    bool modelActive = false;
    bool resetWasDown = false;
    int currentProgram = 1;
    float cpuSmoothed = 0.0f;
    float inPeakHold = 0.0f, outPeakHold = 0.0f;
    float peakDecayPerBlock = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ZeroFbAudioProcessor)
};

} // namespace zerofb
