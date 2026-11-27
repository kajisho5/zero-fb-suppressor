// ZERO-FB - plug-in processor.

#include "PluginProcessor.h"
#include "PluginEditor.h"

// Trained weights are never committed (SPEC 3.2): the file below exists only
// in a local checkout that has run training/export_weights.py --checkpoint.
#if __has_include("nn/EmbeddedWeights.h")
 #include "nn/EmbeddedWeights.h"
 #define ZEROFB_HAS_MODEL 1
#else
 #define ZEROFB_HAS_MODEL 0
#endif

namespace zerofb {

namespace id {
const juce::ParameterID suppress { "suppress", 1 };
const juce::ParameterID mode { "mode", 1 };
const juce::ParameterID dereverb { "dereverb", 1 };
const juce::ParameterID floor { "floor", 1 };
const juce::ParameterID reset { "reset", 1 };
const juce::ParameterID bypass { "bypass", 1 };
} // namespace id

void ZeroFbAudioProcessor::useWeights (const float* payload) noexcept
{
    network.setWeights (payload);
    modelActive = payload != nullptr;
}

juce::AudioProcessorValueTreeState::ParameterLayout ZeroFbAudioProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    const auto percent = AudioParameterFloatAttributes().withLabel ("%");
    layout.add (std::make_unique<AudioParameterFloat> (id::suppress, "SUPPRESS",
                                                       NormalisableRange<float> (0.0f, 100.0f, 0.1f), 75.0f, percent));
    layout.add (std::make_unique<AudioParameterChoice> (id::mode, "MODE", StringArray { "SPEECH", "VOCAL" }, 0));
    layout.add (std::make_unique<AudioParameterFloat> (id::dereverb, "DEREVERB",
                                                       NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f, percent));
    layout.add (std::make_unique<AudioParameterFloat> (id::floor, "FLOOR",
                                                       NormalisableRange<float> (-36.0f, -6.0f, 0.1f), -24.0f,
                                                       AudioParameterFloatAttributes().withLabel ("dB")));
    layout.add (std::make_unique<AudioParameterBool> (id::reset, "RESET", false));
    layout.add (std::make_unique<AudioParameterBool> (id::bypass, "BYPASS", false));
    return layout;
}

ZeroFbAudioProcessor::ZeroFbAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "ZEROFB", createLayout())
{
    suppressValue = apvts.getRawParameterValue (id::suppress.getParamID());
    dereverbValue = apvts.getRawParameterValue (id::dereverb.getParamID());
    floorValue = apvts.getRawParameterValue (id::floor.getParamID());
    modeValue = apvts.getRawParameterValue (id::mode.getParamID());
    resetValue = apvts.getRawParameterValue (id::reset.getParamID());
    bypassValue = apvts.getRawParameterValue (id::bypass.getParamID());
    bypassParam = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter (id::bypass.getParamID()));

   #if ZEROFB_HAS_MODEL
    useWeights (embedded::kWeights);
   #endif
    engine.setEstimator (&network);
    for (auto& g : telemetry.bandGainDb)
        g.store (0.0f);
}

bool ZeroFbAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    const auto mono = juce::AudioChannelSet::mono();
    const auto stereo = juce::AudioChannelSet::stereo();
    return (in == mono && out == mono) || (in == stereo && out == stereo) || (in == mono && out == stereo);
}

void ZeroFbAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    setLatencySamples (ZeroFbEngine::latencySamples());
    engine.prepare (sampleRate, juce::jlimit (1, kMaxChannels, getTotalNumInputChannels()));
    resetWasDown = false;
    cpuSmoothed = 0.0f;
    inPeakHold = outPeakHold = 0.0f;
    // Meter fall time: 20 dB per second.
    const double blocksPerSecond = sampleRate / double (juce::jmax (1, samplesPerBlock));
    peakDecayPerBlock = float (std::pow (10.0, -20.0 / (20.0 * blocksPerSecond)));
    telemetry.sampleRate.store (sampleRate);
    telemetry.sampleRateSupported.store (engine.isSupported());
    telemetry.blockSize.store (samplesPerBlock);
}

void ZeroFbAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    const juce::ScopedNoDenormals noDenormals;
    const auto startTicks = juce::Time::getHighResolutionTicks();

    const int numIn = juce::jmin (getTotalNumInputChannels(), buffer.getNumChannels());
    const int numOut = juce::jmin (getTotalNumOutputChannels(), buffer.getNumChannels());
    const int numSamples = buffer.getNumSamples();
    if (numIn < 1 || numOut < 1 || numSamples == 0)
        return;

    EngineParams params;
    params.suppress = suppressValue->load() * 0.01f;
    params.dereverb = dereverbValue->load() * 0.01f;
    params.floorDb = floorValue->load();
    params.vocal = modeValue->load() >= 0.5f;
    params.bypass = bypassValue->load() >= 0.5f;
    engine.setParams (params);

    const bool resetDown = resetValue->load() >= 0.5f;
    if (resetDown && ! resetWasDown)
        engine.requestReset();
    resetWasDown = resetDown;

    float inPeak = 0.0f;
    for (int c = 0; c < juce::jmin (numIn, kMaxChannels); ++c)
        inPeak = juce::jmax (inPeak, buffer.getMagnitude (c, 0, numSamples));

    const float* in[kMaxChannels] = { buffer.getReadPointer (0), buffer.getReadPointer (juce::jmin (1, numIn - 1)) };
    float* out[kMaxChannels] = { buffer.getWritePointer (0), buffer.getWritePointer (juce::jmin (1, numOut - 1)) };
    engine.process (in, out, juce::jmin (numIn, kMaxChannels), juce::jmin (numOut, kMaxChannels), numSamples);
    for (int c = kMaxChannels; c < numOut; ++c)
        buffer.clear (c, 0, numSamples);

    float outPeak = 0.0f;
    for (int c = 0; c < juce::jmin (numOut, kMaxChannels); ++c)
        outPeak = juce::jmax (outPeak, buffer.getMagnitude (c, 0, numSamples));

    // Telemetry for the editor.
    const float* gains = engine.bandGainsDb();
    std::uint32_t holds = 0;
    for (int b = 0; b < kNumBands; ++b)
    {
        telemetry.bandGainDb[size_t (b)].store (gains[b], std::memory_order_relaxed);
        if (engine.maskSmoother().bandState (b) == MaskSmoother::State::hold)
            holds |= (1u << b);
    }
    telemetry.holdMask.store (holds, std::memory_order_relaxed);
    auto finiteOrZero = [] (float v) { return std::isfinite (v) ? v : 0.0f; };
    inPeakHold = juce::jmax (finiteOrZero (inPeak), inPeakHold * peakDecayPerBlock);
    outPeakHold = juce::jmax (finiteOrZero (outPeak), outPeakHold * peakDecayPerBlock);
    telemetry.inputPeakDb.store (juce::Decibels::gainToDecibels (inPeakHold, -100.0f), std::memory_order_relaxed);
    telemetry.outputPeakDb.store (juce::Decibels::gainToDecibels (outPeakHold, -100.0f), std::memory_order_relaxed);
    telemetry.blockSize.store (numSamples, std::memory_order_relaxed);

    if (! isNonRealtime())
    {
        const double seconds = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - startTicks);
        const double budget = double (numSamples) / juce::jmax (1.0, getSampleRate());
        const float load = float (100.0 * seconds / budget);
        cpuSmoothed += 0.05f * (load - cpuSmoothed);
        telemetry.cpuLoadPercent.store (cpuSmoothed, std::memory_order_relaxed);
    }
}

juce::AudioProcessorEditor* ZeroFbAudioProcessor::createEditor() { return new ZeroFbEditor (*this); }

// ---------------------------------------------------------------------------
// Presets and state
// ---------------------------------------------------------------------------

void ZeroFbAudioProcessor::applyPreset (int index)
{
    if (index < 0 || index >= int (kPresets.size()))
        return;
    const Preset& p = kPresets[size_t (index)];
    auto set = [this] (const juce::ParameterID& pid, float plainValue)
    {
        if (auto* param = apvts.getParameter (pid.getParamID()))
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
            param->endChangeGesture();
        }
    };
    set (id::suppress, p.suppress);
    set (id::dereverb, p.dereverb);
    set (id::floor, p.floorDb);
    set (id::mode, p.vocal ? 1.0f : 0.0f);
    currentProgram = index;
}

int ZeroFbAudioProcessor::matchingPreset() const
{
    const float s = suppressValue->load(), d = dereverbValue->load(), f = floorValue->load();
    const bool vocal = modeValue->load() >= 0.5f;
    for (size_t i = 0; i < kPresets.size(); ++i)
    {
        const Preset& p = kPresets[i];
        if (std::abs (p.suppress - s) < 0.05f && std::abs (p.dereverb - d) < 0.05f
            && std::abs (p.floorDb - f) < 0.05f && p.vocal == vocal)
            return int (i);
    }
    return -1;
}

void ZeroFbAudioProcessor::setCurrentProgram (int index)
{
    if (index != currentProgram)
        applyPreset (index);
}

const juce::String ZeroFbAudioProcessor::getProgramName (int index)
{
    return index >= 0 && index < int (kPresets.size()) ? juce::String (kPresets[size_t (index)].name) : juce::String();
}

void ZeroFbAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
    {
        xml->setAttribute ("state_version", kStateVersion);
        copyXmlToBinary (*xml, destData);
    }
}

void ZeroFbAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        if (! xml->hasTagName (apvts.state.getType()))
            return;
        // Newer state versions keep unknown attributes; known parameters load as usual.
        apvts.replaceState (juce::ValueTree::fromXml (*xml));
        if (auto* reset = apvts.getParameter (id::reset.getParamID()))
            reset->setValueNotifyingHost (0.0f); // a stored RESET must not fire on load
        const int match = matchingPreset();
        if (match >= 0)
            currentProgram = match;
    }
}

} // namespace zerofb

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new zerofb::ZeroFbAudioProcessor(); }
