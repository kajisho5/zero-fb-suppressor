// ZERO-FB - plug-in editor.

#include "PluginEditor.h"

namespace zerofb {

using namespace ui;

namespace {

constexpr float kMeterFloorDb = -36.0f; // IN / OUT: 36 segments, 1 dB each
constexpr int kMeterSegments = 36;

void setParameter (juce::AudioProcessorValueTreeState& apvts, const char* paramId, float plainValue)
{
    if (auto* p = apvts.getParameter (paramId))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 (plainValue));
        p->endChangeGesture();
    }
}

} // namespace

ZeroFbEditor::ZeroFbEditor (ZeroFbAudioProcessor& p) : juce::AudioProcessorEditor (&p), processor (p)
{
    setOpaque (true);
    addAndMakeVisible (analyzer);

    setupKnob (suppress, "suppress", 26.0f);
    setupKnob (dereverb, "dereverb", 15.0f);
    setupKnob (floorKnob, "floor", 15.0f);
    suppress.ring.setTitle ("Suppress");
    dereverb.ring.setTitle ("Dereverb");
    floorKnob.ring.setTitle ("Floor");

    for (auto* b : { &speech, &vocal })
    {
        b->setClickingTogglesState (false);
        addAndMakeVisible (*b);
    }
    speech.onClick = [this] { setParameter (processor.apvts, "mode", 0.0f); refreshButtons(); };
    vocal.onClick = [this] { setParameter (processor.apvts, "mode", 1.0f); refreshButtons(); };

    // RESET is momentary: the parameter is 1 only while the button is held.
    reset.onStateChange = [this]
    {
        if (auto* param = processor.apvts.getParameter ("reset"))
        {
            const float wanted = reset.isDown() ? 1.0f : 0.0f;
            if (std::abs (param->getValue() - wanted) > 0.5f)
                param->setValueNotifyingHost (wanted);
        }
    };
    addAndMakeVisible (reset);

    bypass.setClickingTogglesState (true);
    addAndMakeVisible (bypass);
    bypassAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (processor.apvts, "bypass", bypass);

    for (int i = 0; i < 4; ++i)
    {
        chips[i] = std::make_unique<FlatButton> (kPresets[size_t (i)].chipLabel, FlatButton::Kind::chip);
        chips[i]->setTitle (kPresets[size_t (i)].name);
        chips[i]->onClick = [this, i] { processor.applyPreset (i); refreshButtons(); };
        addAndMakeVisible (*chips[i]);
    }

    setResizable (false, false);
    setSize (kWidth, kHeight);
    frame = readTelemetry();
    analyzer.setFrame (frame);
    refreshButtons();
    startTimerHz (60);
}

ZeroFbEditor::~ZeroFbEditor() { stopTimer(); }

void ZeroFbEditor::setupKnob (Knob& k, const juce::String& paramId, float fontPx)
{
    addAndMakeVisible (k.ring);
    k.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (processor.apvts, paramId, k.ring);
    if (auto* param = processor.apvts.getParameter (paramId))
        k.ring.setDoubleClickReturnValue (true, param->convertFrom0to1 (param->getDefaultValue()));

    k.value.setFont (Fonts::get().font (Face::monoBold, fontPx));
    k.value.setJustificationType (juce::Justification::centred);
    k.value.setColour (juce::Label::textColourId, colour::text1);
    k.value.setColour (juce::Label::backgroundWhenEditingColourId, colour::well);
    k.value.setColour (juce::Label::textWhenEditingColourId, colour::text1);
    k.value.setColour (juce::Label::outlineWhenEditingColourId, colour::catRing);
    k.value.setEditable (true, false, false); // a click on the number opens keyboard entry
    k.value.setBorderSize ({});
    k.value.setMinimumHorizontalScale (1.0f);
    auto show = [&k] { k.value.setText (juce::String (k.ring.getValue(), k.decimals), juce::dontSendNotification); };
    k.ring.onValueChange = show;
    k.value.onTextChange = [&k, show]
    {
        const auto text = k.value.getText().retainCharacters ("-+.0123456789");
        if (text.isNotEmpty())
            k.ring.setValue (text.getDoubleValue(), juce::sendNotificationSync);
        show();
    };
    addAndMakeVisible (k.value);
    show();
}

void ZeroFbEditor::resized()
{
    analyzer.setBounds (26, 67, 460, 190);

    speech.setBounds (448, 19, 68, 23);
    vocal.setBounds (516, 19, 68, 23);
    reset.setBounds (596, 17, 62, 27);
    bypass.setBounds (666, 17, 68, 27);
    for (int i = 0; i < 4; ++i)
        chips[i]->setBounds (26 + i * 117, 271, 109, 44);

    suppress.ring.setBounds (620 - 68, 160 - 68, 136, 136);
    suppress.value.setBounds (620 - 34, 144, 68, 30);
    dereverb.ring.setBounds (563 - 48, 312 - 48, 96, 96);
    dereverb.value.setBounds (563 - 24, 303, 48, 18);
    floorKnob.ring.setBounds (677 - 48, 312 - 48, 96, 96);
    floorKnob.value.setBounds (677 - 24, 303, 48, 18);
}

ui::DisplayFrame ZeroFbEditor::readTelemetry() const
{
    const auto& t = processor.telemetry;
    DisplayFrame f;
    for (int b = 0; b < kNumBands; ++b)
        f.gainDb[size_t (b)] = t.bandGainDb[size_t (b)].load (std::memory_order_relaxed);
    f.holdMask = t.holdMask.load (std::memory_order_relaxed);
    f.inputDb = t.inputPeakDb.load (std::memory_order_relaxed);
    f.outputDb = t.outputPeakDb.load (std::memory_order_relaxed);
    f.cpuPercent = t.cpuLoadPercent.load (std::memory_order_relaxed);
    f.blockSize = t.blockSize.load (std::memory_order_relaxed);
    f.sampleRateSupported = t.sampleRateSupported.load (std::memory_order_relaxed);
    return f;
}

void ZeroFbEditor::showPreviewFrame (const ui::DisplayFrame& f)
{
    previewing = true;
    stopTimer();
    frame = f;
    analyzer.setFrame (frame);
    refreshButtons();
    repaint();
}

void ZeroFbEditor::refreshButtons()
{
    const bool vocalMode = processor.apvts.getRawParameterValue ("mode")->load() >= 0.5f;
    speech.setToggleState (! vocalMode, juce::dontSendNotification);
    vocal.setToggleState (vocalMode, juce::dontSendNotification);
    const int preset = processor.matchingPreset();
    for (int i = 0; i < 4; ++i)
        chips[i]->setToggleState (i == preset, juce::dontSendNotification);
}

void ZeroFbEditor::timerCallback()
{
    frame = readTelemetry();
    analyzer.setFrame (frame);
    refreshButtons();
    repaint (26, 325, 462, 62);                       // meters
    repaint (10, kHeight - 41, kWidth - 20, 31);      // EVO bar
}

void ZeroFbEditor::paintMeter (juce::Graphics& g, float y, const char* label, const juce::String& value,
                               float fraction, bool isReduction) const
{
    const auto& fonts = Fonts::get();
    drawText (g, label, 26.0f, y + 10.0f, colour::text1, fonts.font (Face::uiSemiBold, 12.0f, 0.16f));
    const int lit = juce::roundToInt (juce::jlimit (0.0f, 1.0f, fraction) * float (kMeterSegments));
    for (int k = 0; k < kMeterSegments; ++k)
    {
        juce::Colour c = colour::control;
        if (k < lit)
            c = isReduction ? colour::catRing : (k < 25 ? colour::ok : (k < 32 ? colour::warn : colour::over));
        g.setColour (c);
        g.fillRoundedRectangle (66.0f + float (k) * 10.0f, y, 8.4f, 12.0f, 1.0f);
    }
    drawText (g, value, 486.0f, y + 11.0f, colour::text1, fonts.font (Face::monoBold, 13.0f), juce::Justification::right);
}

void ZeroFbEditor::paint (juce::Graphics& g)
{
    const auto& fonts = Fonts::get();
    const float w = float (kWidth), h = float (kHeight);

    // Road case: steel frame with ball corners.
    g.fillAll (juce::Colour (0xff26272b));
    {
        juce::ColourGradient steel (juce::Colour (0xff5a5d63), 0.0f, 0.0f, juce::Colour (0xff26272b), 0.0f, h, false);
        steel.addColour (0.5, juce::Colour (0xff3a3c41));
        g.setGradientFill (steel);
        g.fillRoundedRectangle (0.0f, 0.0f, w, h, 14.0f);
    }
    fillVertical (g, { 10.0f, 10.0f, w - 20.0f, h - 20.0f }, colour::groundTop, colour::ground, 5.0f);
    g.setColour (colour::well);
    g.drawRoundedRectangle (10.0f, 10.0f, w - 20.0f, h - 20.0f, 5.0f, 1.0f);
    for (const auto corner : { juce::Point<float> (15.0f, 15.0f), { w - 15.0f, 15.0f }, { 15.0f, h - 15.0f }, { w - 15.0f, h - 15.0f } })
    {
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff8a8d93), corner.x - 3.0f, corner.y - 4.0f,
                                                 juce::Colour (0xff2a2b2f), corner.x + 9.0f, corner.y + 9.0f, true));
        g.fillEllipse (corner.x - 9.5f, corner.y - 9.5f, 19.0f, 19.0f);
        g.setColour (juce::Colour (0xff111214));
        g.drawEllipse (corner.x - 9.5f, corner.y - 9.5f, 19.0f, 19.0f, 1.0f);
        g.setColour (colour::well);
        g.fillEllipse (corner.x - 3.0f, corner.y - 3.0f, 6.0f, 6.0f);
    }

    // Toolbar (40 px) and category strip (3 px).
    fillVertical (g, { 10.5f, 10.5f, w - 21.0f, 40.0f }, colour::toolbarTop, colour::toolbar);
    g.setColour (colour::cat);
    g.fillRect (10.5f, 50.5f, w - 21.0f, 3.0f);
    g.setColour (colour::catEar);
    g.fillRoundedRectangle (34.0f, 21.0f, 5.0f, 19.0f, 1.0f);
    g.setColour (colour::cat);
    g.fillRoundedRectangle (40.0f, 21.0f, 5.0f, 19.0f, 1.0f);
    g.setColour (colour::well);
    g.fillEllipse (41.2f, 24.7f, 2.6f, 2.6f);
    g.fillEllipse (41.2f, 33.7f, 2.6f, 2.6f);
    drawText (g, "SW AUDIO", 53.0f, 34.0f, colour::text3, fonts.font (Face::display, 9.0f, 0.22f));
    g.setColour (colour::cat);
    g.fillRoundedRectangle (150.0f, 21.0f, 42.0f, 19.0f, 3.0f);
    drawText (g, "LIVE", 171.0f, 34.5f, colour::ink, fonts.font (Face::monoBold, 11.0f), juce::Justification::horizontallyCentred);
    drawText (g, "ZERO-FB", 202.0f, 34.5f, colour::text1, fonts.font (Face::display, 11.0f, 0.18f));

    // Latency is always on screen (LIVE line rule). The engine adds none.
    g.setColour (colour::ok);
    g.fillEllipse (314.5f, 27.0f, 7.0f, 7.0f);
    drawText (g, "0 smp", 328.0f, 35.0f, colour::text1, fonts.font (Face::monoBold, 13.0f));
    drawText (g, "0.0 ms", 380.0f, 35.0f, colour::text4, fonts.font (Face::mono, 11.0f));

    // MODE frame (the two segments are buttons).
    g.setColour (colour::control);
    g.fillRoundedRectangle (446.0f, 17.0f, 140.0f, 27.0f, 4.0f);
    g.setColour (colour::line);
    g.drawRoundedRectangle (446.5f, 17.5f, 139.0f, 26.0f, 4.0f, 1.0f);

    // Meters.
    const float gr = GrAnalyzerView::maxReductionDb (frame);
    auto level = [] (float db) { return juce::String (juce::roundToInt (juce::jmax (-99.0f, db))); };
    paintMeter (g, 331.0f, "IN", level (frame.inputDb), 1.0f - frame.inputDb / kMeterFloorDb, false);
    paintMeter (g, 351.0f, "GR", level (gr), -gr / GrAnalyzerView::kRangeDb, true);
    paintMeter (g, 371.0f, "OUT", level (frame.outputDb), 1.0f - frame.outputDb / kMeterFloorDb, false);

    // Right column: labels and units (the numbers are editable labels).
    g.setColour (colour::line);
    g.fillRect (496.0f, 67.0f, 1.0f, 319.0f);
    const auto label = fonts.font (Face::uiSemiBold, 12.0f, 0.16f);
    drawText (g, "SUPPRESS", 620.0f, 84.0f, colour::text1, label, juce::Justification::horizontallyCentred);
    drawText (g, "DEREVERB", 563.0f, 252.0f, colour::text1, label, juce::Justification::horizontallyCentred);
    drawText (g, "FLOOR", 677.0f, 252.0f, colour::text1, label, juce::Justification::horizontallyCentred);

    // EVO bar (30 px).
    const float evoY = h - 40.5f;
    fillVertical (g, { 10.5f, evoY, w - 21.0f, 30.0f }, colour::toolbarTop, colour::toolbar);
    g.setColour (colour::line);
    g.fillRect (10.5f, evoY, w - 21.0f, 1.0f);
    g.setColour (colour::cat);
    g.fillRoundedRectangle (34.0f, h - 33.0f, 34.0f, 15.0f, 3.0f);
    drawText (g, "EVO", 51.0f, h - 22.0f, colour::ink, fonts.font (Face::monoBold, 10.0f, 0.08f), juce::Justification::horizontallyCentred);
    const auto evoFont = fonts.font (Face::uiRegular, 12.0f, 0.03f);
    if (! frame.sampleRateSupported)
        drawText (g, "UNSUPPORTED SAMPLE RATE. Use 44.1, 48 or 96 kHz. Audio passes unchanged.", 78.0f, h - 21.0f, colour::warn, evoFont);
    else if (! processor.isModelActive())
        drawText (g, "NO MODEL LOADED. Audio passes unchanged, nothing is suppressed.", 78.0f, h - 21.0f, colour::warn, evoFont);
    else
        drawText (g, "AI mask steers a 32-band minimum-phase EQ. No look-ahead, no added delay.", 78.0f, h - 21.0f, colour::text2, evoFont);
    drawText (g, "CPU " + juce::String (frame.cpuPercent, 1) + " %   BUF " + juce::String (frame.blockSize),
              w - 34.0f, h - 21.0f, colour::text1, fonts.font (Face::monoBold, 11.0f), juce::Justification::right);
}

// The units sit on the knob discs, which are child components.
void ZeroFbEditor::paintOverChildren (juce::Graphics& g)
{
    const auto& fonts = Fonts::get();
    drawText (g, "%", 620.0f, 184.0f, colour::text4, fonts.font (Face::mono, 11.0f), juce::Justification::horizontallyCentred);
    drawText (g, "%", 563.0f, 330.0f, colour::text4, fonts.font (Face::mono, 10.0f), juce::Justification::horizontallyCentred);
    drawText (g, "dB", 677.0f, 330.0f, colour::text4, fonts.font (Face::mono, 10.0f), juce::Justification::horizontallyCentred);
}

} // namespace zerofb
