// ZERO-FB - renders the editor to a PNG without a host.
// Used to compare the real rendering with the design master (SPEC 6.3).
//
//   UiSnapshot <output.png> [scale] [preset 0-3]

#include "PluginEditor.h"

#include <cmath>

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf (stderr, "usage: UiSnapshot <output.png> [scale]\n");
        return 2;
    }
    const float scale = argc > 2 ? juce::String (argv[2]).getFloatValue() : 2.0f;

    juce::ScopedJuceInitialiser_GUI gui;
    int result = 0;
    {
        zerofb::ZeroFbAudioProcessor processor;
        if (argc > 3)
            processor.applyPreset (juce::String (argv[3]).getIntValue());
        std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
        auto* view = dynamic_cast<zerofb::ZeroFbEditor*> (editor.get());
        if (view == nullptr)
            return 1;

        // Fixed, invented values: a feedback cut around 2.5 kHz over a mild
        // dereverb curve. Same picture as the design master.
        zerofb::ui::DisplayFrame frame;
        for (int b = 0; b < zerofb::kNumBands; ++b)
        {
            const float x = float (b);
            float g = -2.0f - 1.5f * std::sin (x * 0.35f) - 3.0f * std::exp (-(x - 6.0f) * (x - 6.0f) / 18.0f);
            if (b == 20) g = -17.0f;
            if (b == 19 || b == 21) g = -7.5f;
            if (b == 26) g = -11.0f;
            frame.gainDb[size_t (b)] = g;
        }
        frame.holdMask = (1u << 20) | (1u << 26);
        frame.inputDb = -9.0f;
        frame.outputDb = -12.0f;
        frame.cpuPercent = 3.4f;
        frame.blockSize = 64;
        frame.sampleRateSupported = true;
        view->showPreviewFrame (frame);

        const auto image = view->createComponentSnapshot (view->getLocalBounds(), true, scale);
        juce::File out = juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]);
        out.deleteFile();
        juce::FileOutputStream stream (out);
        juce::PNGImageFormat png;
        if (! stream.openedOk() || ! png.writeImageToStream (image, stream))
        {
            std::fprintf (stderr, "could not write %s\n", argv[1]);
            result = 1;
        }
    }
    return result;
}
