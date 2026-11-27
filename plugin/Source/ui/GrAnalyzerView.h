// ZERO-FB - 32-band gain reduction display (SPEC 6.3).
#pragma once

#include "CustomLookAndFeel.h"
#include "../dsp/BandLayout.h"

#include <array>

namespace zerofb::ui {

/** What the editor shows in one frame. Filled from Telemetry by the timer,
    or with fixed preview values by the snapshot tool. */
struct DisplayFrame
{
    std::array<float, kNumBands> gainDb {}; // <= 0
    std::uint32_t holdMask = 0;
    float inputDb = -100.0f, outputDb = -100.0f;
    float cpuPercent = 0.0f;
    int blockSize = 0;
    bool sampleRateSupported = true;
};

class GrAnalyzerView final : public juce::Component
{
public:
    static constexpr float kRangeDb = 24.0f;

    GrAnalyzerView()
    {
        setInterceptsMouseClicks (false, false);
        setOpaque (false);
    }

    void setFrame (const DisplayFrame& f)
    {
        frame = f;
        repaint();
    }

    /** Deepest band that is holding a feedback cut, or -1. */
    static int ringingBand (const DisplayFrame& f)
    {
        int best = -1;
        for (int b = 0; b < kNumBands; ++b)
            if ((f.holdMask >> b) & 1u)
                if (best < 0 || f.gainDb[size_t (b)] < f.gainDb[size_t (best)])
                    best = b;
        return best;
    }

    static float maxReductionDb (const DisplayFrame& f)
    {
        float deepest = 0.0f;
        for (float g : f.gainDb)
            deepest = juce::jmin (deepest, g);
        return deepest;
    }

    static juce::String frequencyText (double hz)
    {
        return hz >= 1000.0 ? juce::String (hz / 1000.0, 2) + " kHz" : juce::String (juce::roundToInt (hz)) + " Hz";
    }

    void paint (juce::Graphics& g) override
    {
        const auto& fonts = Fonts::get();
        const auto r = getLocalBounds().toFloat();
        fillVertical (g, r, colour::displayTop, colour::display, 6.0f);
        g.setColour (colour::line);
        g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);

        drawText (g, "GAIN REDUCTION", 12.0f, 18.0f, colour::text3, fonts.font (Face::uiMedium, 11.0f, 0.24f));
        drawText (g, "MAX " + juce::String (maxReductionDb (frame), 1) + " dB", r.getWidth() - 12.0f, 19.0f,
                  colour::text1, fonts.font (Face::monoBold, 13.0f), juce::Justification::right);

        // Plot area: 24 dB over 126 px.
        const float x0 = 40.0f, x1 = r.getWidth() - 12.0f, y0 = 34.0f, h = 126.0f;
        const char* const labels[] = { "0", "-6", "-12", "-18", "-24" };
        for (int i = 0; i < 5; ++i)
        {
            const float y = y0 + float (i) * 0.25f * h;
            g.setColour (colour::line);
            if (i == 4)
            {
                const float dashes[] = { 3.0f, 3.0f };
                g.drawDashedLine ({ x0, y, x1, y }, dashes, 2, 1.0f);
            }
            else
            {
                g.drawLine (x0, y, x1, y, 1.0f);
            }
            drawText (g, labels[i], x0 - 6.0f, y + 3.5f, colour::text4, fonts.font (Face::mono, 10.0f), juce::Justification::right);
        }

        const float barW = 10.0f;
        const float step = (x1 - x0 - barW) / float (kNumBands - 1);
        const int ring = ringingBand (frame);
        for (int b = 0; b < kNumBands; ++b)
        {
            const float reduction = juce::jlimit (0.0f, kRangeDb, -frame.gainDb[size_t (b)]);
            const float barH = reduction / kRangeDb * h;
            const float x = x0 + float (b) * step;
            const bool holding = ((frame.holdMask >> b) & 1u) != 0;
            const juce::Colour c = holding ? colour::warn : colour::catRing;
            g.setColour (c.withAlpha (holding ? 0.34f : 0.26f));
            g.fillRect (x, y0, barW, barH);
            if (barH >= 0.5f)
            {
                g.setColour (c);
                g.fillRect (x, y0 + juce::jmax (0.0f, barH - 2.0f), barW, 2.0f);
            }
        }

        // Frequency scale: positions follow the ERB band layout.
        const BandLayout layout;
        auto bandPosition = [&layout] (double hz)
        {
            auto erbs = [] (double f) { return 21.4 * std::log10 (0.00437 * f + 1.0); };
            return float ((erbs (hz) - erbs (layout.fc[0])) / (erbs (layout.fc[kNumBands - 1]) - erbs (layout.fc[0])) * (kNumBands - 1));
        };
        const std::pair<double, const char*> ticks[] = { { 60.0, "60" }, { 250.0, "250" }, { 1000.0, "1k" }, { 4000.0, "4k" }, { 15000.0, "15k" } };
        for (const auto& t : ticks)
            drawText (g, t.second, x0 + barW * 0.5f + bandPosition (t.first) * step, r.getHeight() - 8.0f,
                      colour::text4, fonts.font (Face::mono, 10.0f), juce::Justification::horizontallyCentred);

        if (ring >= 0)
        {
            const juce::String text = "RING " + frequencyText (layout.fc[size_t (ring)]);
            const float boxW = 106.0f, boxH = 20.0f;
            float bx = x0 + float (ring) * step + 16.0f;
            if (bx + boxW > x1)
                bx = x0 + float (ring) * step - 6.0f - boxW; // keep the label inside the window
            const float by = y0 + 78.0f;
            g.setColour (colour::ground);
            g.fillRoundedRectangle (bx, by, boxW, boxH, 3.0f);
            g.setColour (colour::warn);
            g.drawRoundedRectangle (bx + 0.5f, by + 0.5f, boxW - 1.0f, boxH - 1.0f, 3.0f, 1.0f);
            drawText (g, text, bx + boxW * 0.5f, by + 14.0f, colour::warn, fonts.font (Face::monoBold, 11.0f),
                      juce::Justification::horizontallyCentred);
        }
    }

private:
    DisplayFrame frame;
};

} // namespace zerofb::ui
