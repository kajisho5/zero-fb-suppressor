// ZERO-FB - plug-in editor: SW AUDIO LIVE line look, 760 x 440 px (SPEC 6.3).
#pragma once

#include "PluginProcessor.h"
#include "ui/CustomLookAndFeel.h"
#include "ui/GrAnalyzerView.h"

namespace zerofb {

class ZeroFbEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    static constexpr int kWidth = 760, kHeight = 440;

    explicit ZeroFbEditor (ZeroFbAudioProcessor&);
    ~ZeroFbEditor() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

    /** Shows fixed values instead of live telemetry (used by the snapshot
        tool to compare the real rendering with the design master). */
    void showPreviewFrame (const ui::DisplayFrame& frame);

private:
    struct Knob
    {
        Knob (float ringRadius, float segW, float segH, float disc) : ring (ringRadius, segW, segH, disc) {}
        ui::LedRingKnob ring;
        juce::Label value;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
        int decimals = 0;
    };

    void timerCallback() override;
    void setupKnob (Knob&, const juce::String& paramId, float fontPx);
    void refreshButtons();
    void paintMeter (juce::Graphics&, float y, const char* label, const juce::String& value, float fraction, bool isReduction) const;
    ui::DisplayFrame readTelemetry() const;

    ZeroFbAudioProcessor& processor;
    ui::DisplayFrame frame;
    bool previewing = false;

    ui::GrAnalyzerView analyzer;
    Knob suppress { 58.0f, 5.5f, 13.0f, 42.0f };
    Knob dereverb { 40.0f, 4.0f, 10.0f, 28.0f };
    Knob floorKnob { 40.0f, 4.0f, 10.0f, 28.0f };
    ui::FlatButton speech { "SPEECH", ui::FlatButton::Kind::segment };
    ui::FlatButton vocal { "VOCAL", ui::FlatButton::Kind::segment };
    ui::FlatButton reset { "RESET", ui::FlatButton::Kind::toolbar };
    ui::FlatButton bypass { "BYPASS", ui::FlatButton::Kind::toolbar };
    std::unique_ptr<ui::FlatButton> chips[4];
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ZeroFbEditor)
};

} // namespace zerofb
