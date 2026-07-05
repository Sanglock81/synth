// ============================================================================
// Master-gain smoothing (zipper). Master gain multiplies the whole output, so a
// hard step is a genuine click. Test-first: step the gain param hard every block
// and require the block-boundary discontinuity stays close to the signal's own
// per-sample variation (i.e. no click).
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "PluginProcessor.h"
#include <vector>
#include <cmath>

namespace
{
    float maxBoundaryDelta (const std::vector<float>& x, std::size_t block)
    {
        float m = 0.0f;
        for (std::size_t i = block; i < x.size(); i += block)
            m = std::max (m, std::abs (x[i] - x[i - 1]));
        return m;
    }
    float maxInteriorDelta (const std::vector<float>& x, std::size_t block)
    {
        float m = 0.0f;
        for (std::size_t i = 1; i < x.size(); ++i)
            if (i % block != 0) m = std::max (m, std::abs (x[i] - x[i - 1]));
        return m;
    }
}

TEST_CASE ("master gain is smoothed: hard steps do not click", "[plugin][smoothing][zipper]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    const int block = 64;
    p.prepareToPlay (48000.0, block);

    auto* gain = p.apvts.getParameter ("master_gain");

    // Sustained note.
    {
        juce::AudioBuffer<float> buf (2, block);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 45, 0.9f), 0);
        buf.clear();
        p.processBlock (buf, midi);
    }

    const int blocks = 200;
    std::vector<float> out;
    out.reserve ((std::size_t) (block * blocks));
    for (int b = 0; b < blocks; ++b)
    {
        gain->setValueNotifyingHost ((b & 1) ? 0.15f : 0.85f);   // hard step every block
        juce::AudioBuffer<float> buf (2, block);
        buf.clear();
        juce::MidiBuffer midi;
        p.processBlock (buf, midi);
        const float* ch = buf.getReadPointer (0);
        for (int i = 0; i < block; ++i) out.push_back (ch[i]);
    }

    const float boundary = maxBoundaryDelta (out, (std::size_t) block);
    const float interior = maxInteriorDelta (out, (std::size_t) block);
    INFO ("boundary maxDelta=" << boundary << "  interior maxDelta=" << interior);

    // Smoothed: the step is a per-sample ramp, so boundary deltas are no worse
    // than the signal's own inter-sample variation. Unsmoothed: a ~0.7x gain
    // jump makes boundary >> interior.
    REQUIRE (boundary <= interior * 1.5f + 0.02f);
}
