// Synth — Copyright © 2026 John L Farmer. Licensed under AGPLv3; see LICENSE.
// ============================================================================
// #96 Unison (plugin) — the osc_unison param flows through buildVoiceParams, the
// engine dispatches unison voices to the stereo bus, and the output widens. Count 1
// stays the mono path (goldens cover bit-exactness at the DSP layer).
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "PluginProcessor.h"
#include <cmath>

namespace
{
    // Play middle C for `blocks` and return (stereo spread = sum|L-R|, peak).
    std::pair<double, float> playMeasure (VASynthProcessor& p, int unison, int blocks = 30)
    {
        p.loadInitPreset();                    // clean base (1 osc, no width FX) — the default patch may be rich/wide
        *dynamic_cast<juce::AudioParameterInt*> (p.apvts.getParameter (ParamID::oscUnison)) = unison;
        p.apvts.getParameter (ParamID::oscUnisonDetune)->setValueNotifyingHost (0.6f);
        p.apvts.getParameter (ParamID::oscUnisonWidth)->setValueNotifyingHost (0.9f);
        p.prepareToPlay (48000.0, 128);
        double spread = 0.0; float peak = 0.0f;
        for (int b = 0; b < blocks; ++b)
        {
            juce::AudioBuffer<float> buf (2, 128); buf.clear();
            juce::MidiBuffer m; if (b == 0) m.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
            p.processBlock (buf, m);
            if (b >= 6)
            {
                const float* L = buf.getReadPointer (0);
                const float* R = buf.getReadPointer (1);
                for (int i = 0; i < 128; ++i)
                {
                    spread += std::abs ((double) L[i] - R[i]);
                    peak = std::max (peak, std::max (std::abs (L[i]), std::abs (R[i])));
                }
            }
        }
        return { spread, peak };
    }
}

TEST_CASE ("unison: a 7-voice stack widens the output vs a single voice", "[plugin][unison]")
{
    juce::ScopedJuceInitialiser_GUI init;

    float peak1 = 0.0f, peak7 = 0.0f; double spread1 = 0.0, spread7 = 0.0;
    { VASynthProcessor p; auto r = playMeasure (p, 1); spread1 = r.first; peak1 = r.second; }
    { VASynthProcessor p; auto r = playMeasure (p, 7); spread7 = r.first; peak7 = r.second; }

    INFO ("spread1=" << spread1 << " spread7=" << spread7 << " peak1=" << peak1 << " peak7=" << peak7);
    REQUIRE (peak7 > 0.02f);                 // audible
    REQUIRE (peak7 < 1.0f);                   // the output stays bounded (safety-clipper territory not exceeded)
    REQUIRE (spread7 > spread1 * 3.0);       // the stack is clearly wider than the single voice
}

TEST_CASE ("unison: the count param round-trips through state", "[plugin][unison][state]")
{
    juce::ScopedJuceInitialiser_GUI init;
    juce::MemoryBlock blob;
    {
        VASynthProcessor src;
        *dynamic_cast<juce::AudioParameterInt*> (src.apvts.getParameter (ParamID::oscUnison)) = 5;
        src.getStateInformation (blob);
    }
    VASynthProcessor dst;
    dst.setStateInformation (blob.getData(), (int) blob.getSize());
    REQUIRE ((int) dst.apvts.getRawParameterValue (ParamID::oscUnison)->load() == 5);
}
