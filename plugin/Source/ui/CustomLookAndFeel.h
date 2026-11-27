// ZERO-FB - colours, fonts and controls of the SW AUDIO LIVE line look
// (SPEC 6.3). Geometry follows docs/ui_master.svg, 760 x 440 px.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <BinaryData.h>

namespace zerofb::ui {

namespace colour {
inline const juce::Colour ground { 0xff101113 }, groundTop { 0xff17181b };
inline const juce::Colour toolbar { 0xff0f1012 }, toolbarTop { 0xff1b1c1f };
inline const juce::Colour deck { 0xff151619 }, display { 0xff08090a }, displayTop { 0xff0e1012 };
inline const juce::Colour control { 0xff222327 }, line { 0xff2f3136 }, well { 0xff070708 };
inline const juce::Colour text1 { 0xffe8e8e8 }, text2 { 0xffcacaca }, text3 { 0xffb8b8b8 }, text4 { 0xffa8a8a8 };
inline const juce::Colour ink { 0xff0c0c0d };
// Category: Restore
inline const juce::Colour cat { 0xff6aa832 }, catRing { 0xff8fd14a }, catEar { 0xff335a12 };
// State colours (never used as category colour)
inline const juce::Colour ok { 0xff2bd14a }, warn { 0xfff0c93d }, over { 0xffe0443e };
} // namespace colour

/** Embedded typefaces (all SIL OFL 1.1). Sizes are CSS-style px (em size). */
class Fonts
{
public:
    enum class Face { display, uiRegular, uiMedium, uiSemiBold, uiBold, mono, monoBold };

    static const Fonts& get()
    {
        static const Fonts instance;
        return instance;
    }

    /** tracking is in em, as CSS letter-spacing. */
    juce::Font font (Face face, float px, float tracking = 0.0f) const
    {
        return juce::Font (juce::FontOptions (faces[size_t (face)]).withPointHeight (px).withKerningFactor (tracking));
    }

private:
    Fonts()
    {
        using namespace BinaryData;
        auto load = [] (const char* data, int size) { return juce::Typeface::createSystemTypefaceFor (data, size_t (size)); };
        faces[size_t (Face::display)] = load (MichromaRegular_ttf, MichromaRegular_ttfSize);
        faces[size_t (Face::uiRegular)] = load (BarlowCondensedRegular_ttf, BarlowCondensedRegular_ttfSize);
        faces[size_t (Face::uiMedium)] = load (BarlowCondensedMedium_ttf, BarlowCondensedMedium_ttfSize);
        faces[size_t (Face::uiSemiBold)] = load (BarlowCondensedSemiBold_ttf, BarlowCondensedSemiBold_ttfSize);
        faces[size_t (Face::uiBold)] = load (BarlowCondensedBold_ttf, BarlowCondensedBold_ttfSize);
        faces[size_t (Face::mono)] = load (SpaceMonoRegular_ttf, SpaceMonoRegular_ttfSize);
        faces[size_t (Face::monoBold)] = load (SpaceMonoBold_ttf, SpaceMonoBold_ttfSize);
    }

    juce::Typeface::Ptr faces[7];
};

using Face = Fonts::Face;

/** Text positioned by its baseline, as in the SVG master. */
inline void drawText (juce::Graphics& g, const juce::String& text, float x, float baseline, juce::Colour c,
                      const juce::Font& font, juce::Justification just = juce::Justification::left)
{
    g.setColour (c);
    g.setFont (font);
    g.drawSingleLineText (text, juce::roundToInt (x), juce::roundToInt (baseline), just);
}

inline void fillVertical (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour top, juce::Colour bottom, float radius = 0.0f)
{
    g.setGradientFill (juce::ColourGradient (top, 0.0f, r.getY(), bottom, 0.0f, r.getBottom(), false));
    if (radius > 0.0f) g.fillRoundedRectangle (r, radius); else g.fillRect (r);
}

/** 32-segment LED ring (LIVE line). The value is always shown as a number by
    the editor; the ring is the coarse read-out. */
class LedRingKnob final : public juce::Slider
{
public:
    LedRingKnob (float ringRadius, float segW, float segH, float discRadius)
        : radius (ringRadius), segmentW (segW), segmentH (segH), disc (discRadius)
    {
        setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        setMouseDragSensitivity (220);
        // Shift or Cmd / Ctrl while dragging: fine adjustment (SPEC 6.2).
        setVelocityModeParameters (0.6, 1, 0.05, true,
                                   static_cast<juce::ModifierKeys::Flags> (juce::ModifierKeys::shiftModifier | juce::ModifierKeys::commandModifier));
        setWantsKeyboardFocus (true);
    }

    void paint (juce::Graphics& g) override
    {
        const auto centre = getLocalBounds().toFloat().getCentre();
        const double proportion = valueToProportionOfLength (getValue());
        const int lit = juce::roundToInt (proportion * 32.0);
        juce::Path segment;
        segment.addRoundedRectangle (-segmentW * 0.5f, -radius - segmentH * 0.5f, segmentW, segmentH, 1.2f);
        for (int k = 0; k < 32; ++k)
        {
            const float degrees = -135.0f + 270.0f * float (k) / 31.0f;
            g.setColour (k < lit ? colour::catRing : colour::control);
            g.fillPath (segment, juce::AffineTransform::rotation (juce::degreesToRadians (degrees))
                                     .translated (centre.x, centre.y));
        }
        g.setColour (colour::deck);
        g.fillEllipse (centre.x - disc, centre.y - disc, disc * 2.0f, disc * 2.0f);
        g.setColour (hasKeyboardFocus (true) ? colour::catRing : colour::line);
        g.drawEllipse (centre.x - disc, centre.y - disc, disc * 2.0f, disc * 2.0f, 1.0f);
    }

private:
    float radius, segmentW, segmentH, disc;
};

/** Flat button in three flavours: toolbar button, MODE segment, preset chip. */
class FlatButton final : public juce::Button
{
public:
    enum class Kind { toolbar, segment, chip };

    FlatButton (const juce::String& label, Kind k) : juce::Button (label), kind (k)
    {
        setButtonText (label);
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        const auto r = getLocalBounds().toFloat();
        const bool on = getToggleState() || (kind != Kind::segment && down && ! isToggleable());
        const float radius = kind == Kind::chip ? 8.0f : (kind == Kind::segment ? 3.0f : 4.0f);

        if (kind == Kind::segment)
        {
            if (on)
            {
                g.setColour (colour::catRing);
                g.fillRoundedRectangle (r.reduced (0.0f, 0.0f), radius);
            }
        }
        else
        {
            g.setColour (on ? colour::catRing : (highlighted ? colour::control.brighter (0.08f) : colour::control));
            g.fillRoundedRectangle (r.reduced (0.5f), radius);
            if (! on)
            {
                g.setColour (hasKeyboardFocus (true) ? colour::catRing : colour::line);
                g.drawRoundedRectangle (r.reduced (0.5f), radius, 1.0f);
            }
        }

        const auto& fonts = Fonts::get();
        const auto font = kind == Kind::chip ? fonts.font (Face::uiBold, 14.0f, 0.10f)
                        : fonts.font (on && kind == Kind::segment ? Face::uiBold : Face::uiSemiBold, 12.0f, 0.12f);
        const juce::Colour textColour = on ? colour::ink : (kind == Kind::segment ? colour::text2 : colour::text1);
        const float baseline = r.getCentreY() + (kind == Kind::chip ? 5.0f : 4.5f);
        drawText (g, getButtonText(), r.getCentreX(), baseline, textColour, font, juce::Justification::horizontallyCentred);
    }

private:
    Kind kind;
};

} // namespace zerofb::ui
