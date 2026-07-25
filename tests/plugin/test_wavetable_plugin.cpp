// ============================================================================
// #95 Wavetable (3b-plugin) — WT is a real 5th wave option wired to the factory
// bank. Covers: the normalized-value PIN (appending "WT" must not silently retarget
// the RANDOM/init wave call sites), WT actually sounding, the table choice changing
// the timbre, and a WT patch surviving save/load.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginProcessor.h"
#include <cmath>

namespace
{
    int waveIndex (VASynthProcessor& p, const char* id)
    { return (int) std::lround (p.apvts.getRawParameterValue (id)->load()); }

    float renderPeak (VASynthProcessor& p, int blocks = 24)
    {
        p.prepareToPlay (48000.0, 128);
        float peak = 0.0f;
        for (int b = 0; b < blocks; ++b)
        {
            juce::AudioBuffer<float> buf (2, 128); buf.clear();
            juce::MidiBuffer m; if (b == 0) m.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
            p.processBlock (buf, m);
            if (b >= 6) peak = std::max (peak, buf.getMagnitude (0, 128));
        }
        return peak;
    }
}

TEST_CASE ("WT pin: the 5-option wave normalized->index mapping is exactly Saw..WT", "[plugin][wt][pin]")
{
    // Appending "WT" shifted the choice mapping; every RANDOM/init `setValueNotifyingHost` on a wave
    // relies on these exact normalized values. If this drifts, patches silently pick the wrong wave.
    juce::ScopedJuceInitialiser_GUI init;
    VASynthProcessor p;
    auto* wave = p.apvts.getParameter (ParamID::osc1Wave);
    auto idxAt = [&] (float norm) { wave->setValueNotifyingHost (norm); return waveIndex (p, ParamID::osc1Wave); };
    REQUIRE (idxAt (0.0f)  == 0);   // Saw
    REQUIRE (idxAt (0.25f) == 1);   // Square
    REQUIRE (idxAt (0.5f)  == 2);   // Triangle
    REQUIRE (idxAt (0.75f) == 3);   // Sine   (the old "1.0f = last" sites now use 0.75)
    REQUIRE (idxAt (1.0f)  == 4);   // WT
}

TEST_CASE ("WT pin: RANDOM never lands an osc on WT (no silent table-less WT)", "[plugin][wt][pin][random]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VASynthProcessor p;
    juce::Random rng (12345);
    for (int i = 0; i < 40; ++i)
    {
        p.randomizeSound (rng);
        for (const char* id : { ParamID::osc1Wave, ParamID::osc2Wave, ParamID::osc3Wave })
            REQUIRE (waveIndex (p, id) <= 3);   // Saw..Sine only; RANDOM leaves WT to the deliberate picker
    }
}

TEST_CASE ("WT sounds: a WT osc plays its factory table", "[plugin][wt]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VASynthProcessor p;
    p.apvts.getParameter (ParamID::osc1Wave)->setValueNotifyingHost (1.0f);   // WT
    p.apvts.getParameter (ParamID::osc2On)->setValueNotifyingHost (0.0f);
    p.apvts.getParameter (ParamID::osc3On)->setValueNotifyingHost (0.0f);
    REQUIRE (renderPeak (p) > 0.02f);          // the Analog table (default) is audible
}

TEST_CASE ("WT table choice changes the timbre", "[plugin][wt]")
{
    juce::ScopedJuceInitialiser_GUI init;
    auto spectrumTilt = [] (float kindNorm)
    {
        VASynthProcessor p;
        p.apvts.getParameter (ParamID::osc1Wave)->setValueNotifyingHost (1.0f);   // WT
        p.apvts.getParameter (ParamID::osc1WtKind)->setValueNotifyingHost (kindNorm);
        p.apvts.getParameter (ParamID::osc2On)->setValueNotifyingHost (0.0f);
        p.apvts.getParameter (ParamID::osc3On)->setValueNotifyingHost (0.0f);
        p.apvts.getParameter (ParamID::filterCutoff)->setValueNotifyingHost (1.0f);   // open filter
        p.prepareToPlay (48000.0, 512);
        juce::AudioBuffer<float> buf (2, 512); double e = 0.0, hi = 0.0;
        for (int b = 0; b < 40; ++b) { buf.clear(); juce::MidiBuffer m; if (b == 0) m.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
            p.processBlock (buf, m);
            if (b >= 10) { const float* d = buf.getReadPointer (0);
                for (int i = 1; i < 512; ++i) { e += (double) d[i] * d[i]; const double df = d[i] - d[i-1]; hi += df * df; } } }
        return e > 1e-9 ? hi / e : 0.0;   // high-freq content proxy (differences energy / total)
    };
    const double analog  = spectrumTilt (0.0f);          // kind 0 Analog
    const double digital = spectrumTilt (1.0f);          // kind 3 Digital
    INFO ("Analog tilt=" << analog << " Digital tilt=" << digital);
    REQUIRE (std::max (analog, digital) > std::min (analog, digital) * 1.5);   // the two tables sound clearly different
}

TEST_CASE ("WT patch survives save/load (wave + table choice persist)", "[plugin][wt][state]")
{
    juce::ScopedJuceInitialiser_GUI init;
    juce::MemoryBlock blob;
    {
        VASynthProcessor src;
        src.apvts.getParameter (ParamID::osc1Wave)->setValueNotifyingHost (1.0f);       // WT
        src.apvts.getParameter (ParamID::osc1WtKind)->setValueNotifyingHost (1.0f);     // Digital
        src.apvts.getParameter (ParamID::osc1WtPos)->setValueNotifyingHost (0.5f);
        src.getStateInformation (blob);
    }
    VASynthProcessor dst;
    dst.setStateInformation (blob.getData(), (int) blob.getSize());
    REQUIRE (waveIndex (dst, ParamID::osc1Wave)   == 4);   // WT
    REQUIRE (waveIndex (dst, ParamID::osc1WtKind) == 3);   // Digital
    REQUIRE (dst.apvts.getRawParameterValue (ParamID::osc1WtPos)->load() == Catch::Approx (0.5f).margin (0.01));
    REQUIRE (renderPeak (dst) > 0.02f);                    // and it still sounds after reload
}
