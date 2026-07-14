// ============================================================================
// Regression: the 808 kick is ENGINE-CLEAN. A hands-on report of an HF click/pop was
// investigated at length; measured through the real processor, the kick has no meaningful
// sample-to-sample discontinuity (far below the project's kClick=0.35 standard) on a single
// hit OR under rapid sequencer-style retriggers, and its render is block-size-independent
// (the pitch env is correctly control-rated at 16 samples). So the engine is not the source
// — this locks that in; a future engine change that made the kick click would fail here.
// The transient CHARACTER (fast attack + fltenv->pitch drop) is a preset-voicing matter.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "PluginProcessor.h"
#include <cmath>

namespace
{
    constexpr float kClick = 0.35f;   // same discontinuity standard as test_click_torture

    struct Scan { float maxJump = 0.0f, peak = 0.0f; bool finite = true; };
    Scan scan (const std::vector<float>& x)
    {
        Scan s;
        for (std::size_t i = 0; i < x.size(); ++i)
        {
            if (! std::isfinite (x[i])) s.finite = false;
            s.peak = std::max (s.peak, std::abs (x[i]));
            if (i > 0) s.maxJump = std::max (s.maxJump, std::abs (x[i] - x[i-1]));
        }
        return s;
    }
}

TEST_CASE ("808 kick is engine-clean on a single hit", "[plugin][drums][click]")
{
    const int block = 128;
    VASynthProcessor p; p.prepareToPlay (48000.0, block);
    p.loadFactoryPreset ("Kick 808");

    juce::AudioBuffer<float> buf (2, block); juce::MidiBuffer m;
    std::vector<float> mono;
    p.routeNoteOn (36, 1.0f, 0);
    for (int b = 0; b < 24; ++b) { buf.clear(); m.clear(); p.processBlock (buf, m);
        const float* L = buf.getReadPointer (0); for (int i = 0; i < block; ++i) mono.push_back (L[i]); }

    const auto s = scan (mono);
    INFO ("single-hit maxJump=" << s.maxJump << " peak=" << s.peak);
    REQUIRE (s.finite);
    REQUIRE (s.peak > 0.0f);
    REQUIRE (s.maxJump < kClick);       // no discontinuity (measured ~0.002)
}

TEST_CASE ("808 kick is engine-clean under rapid retriggers", "[plugin][drums][click]")
{
    const int block = 128;
    VASynthProcessor p; p.prepareToPlay (48000.0, block);
    p.setPartKit (1, p.factoryKit ("808 Basics"));   // kick = pad 0 (trigger note 36)

    juce::AudioBuffer<float> buf (2, block); juce::MidiBuffer m;
    std::vector<float> mono;
    for (int b = 0; b < 60; ++b)
    {
        buf.clear(); m.clear();
        if (b % 6 == 0) p.routeNoteOn (36, 1.0f, 1);  // retrigger every ~16 ms
        p.processBlock (buf, m);
        const float* L = buf.getReadPointer (0); for (int i = 0; i < block; ++i) mono.push_back (L[i]);
    }

    const auto s = scan (mono);
    INFO ("retrigger maxJump=" << s.maxJump << " peak=" << s.peak);
    REQUIRE (s.finite);
    REQUIRE (s.maxJump < kClick);       // no retrigger discontinuity (measured ~0.010)
}
