// ============================================================================
// Sequencer multitimbral separation: the seq plays ONLY its target part (with that
// part's own sound), and changing the target mid-sequence must not hang a note on the
// old part. (The seq dispatches note-on then, a gate later, note-off; if the target
// changed between them, the note-off would reach the new part and strand the old one.)
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "PluginProcessor.h"
#include <cmath>

namespace
{
    void s01 (VASynthProcessor& p, const char* id, float v)
    { p.apvts.getParameter (id)->setValueNotifyingHost (v); }
    void setVal (VASynthProcessor& p, const char* id, float v)
    { p.apvts.getParameter (id)->setValueNotifyingHost (p.apvts.getParameter (id)->convertTo0to1 (v)); }

    double seqEnergy (VASynthProcessor& p, int blocks)
    {
        juce::AudioBuffer<float> buf (2, 128); juce::MidiBuffer m; double e = 0.0;
        for (int b = 0; b < blocks; ++b) { buf.clear(); p.processBlock (buf, m); e += buf.getRMSLevel (0, 0, 128); }
        return e;
    }
    float tailPeak (VASynthProcessor& p, int settle = 250, int measure = 120)
    {
        juce::AudioBuffer<float> buf (2, 128); juce::MidiBuffer m;
        for (int b = 0; b < settle; ++b) { buf.clear(); p.processBlock (buf, m); }
        float pk = 0.0f;
        for (int b = 0; b < measure; ++b)
        {
            buf.clear(); p.processBlock (buf, m);
            for (int i = 0; i < buf.getNumSamples(); ++i)
                pk = std::max ({ pk, std::abs (buf.getSample (0, i)), std::abs (buf.getSample (1, i)) });
        }
        return pk;
    }

    void armRow0 (VASynthProcessor& p, int note)
    {
        p.setSeqNote (0, note);
        for (int s = 0; s < VASynthProcessor::kSeqSteps; ++s) p.setSeqCell (0, s, 1);
    }
}

TEST_CASE ("seq plays only its target part (muting the target silences it)", "[plugin][seq][multitimbral]")
{
    const char* lvl[4] { ParamID::part0Level, ParamID::part1Level, ParamID::part2Level, ParamID::part3Level };
    for (int target = 0; target < 4; ++target)
    {
        VASynthProcessor p; p.prepareToPlay (48000.0, 128);
        armRow0 (p, 60);
        s01 (p, ParamID::seqTarget, (float) target / 3.0f);
        s01 (p, ParamID::tempo, 0.9f);
        s01 (p, ParamID::seqOn, 1.0f);
        REQUIRE (seqEnergy (p, 60) > 0.0);         // audible with the target at unity
        s01 (p, lvl[target], 0.0f);                // mute the target
        REQUIRE (tailPeak (p) < 1.0e-4f);          // steady state silent (nothing leaks to another part)
    }
}

TEST_CASE ("seq target uses its OWN baked sound, not the focused part's", "[plugin][seq][multitimbral]")
{
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    REQUIRE (p.setPartPreset (2, "Fat Saw Bass"));   // bake an audible sound on the target (P3)
    p.loadInitPreset();                              // focused part (P1)...
    s01 (p, ParamID::osc1On, 0.0f); s01 (p, ParamID::osc2On, 0.0f);
    s01 (p, ParamID::osc3On, 0.0f); s01 (p, ParamID::noiseLevel, 0.0f);   // ...made silent
    armRow0 (p, 48);
    s01 (p, ParamID::seqTarget, 2.0f / 3.0f);        // target P3
    s01 (p, ParamID::tempo, 0.9f);
    s01 (p, ParamID::seqOn, 1.0f);
    REQUIRE (seqEnergy (p, 120) > 0.0);              // audible -> uses P3's sound, not P1's silence
}

TEST_CASE ("no hang: changing the seq target mid-sequence releases the old part's note", "[plugin][seq][stuck]")
{
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    p.loadInitPreset();
    setVal (p, ParamID::ampRelease, 0.02f);
    armRow0 (p, 60);
    setVal (p, ParamID::seqGate, 0.9f);              // long gate -> a note is mid-flight at the switch
    setVal (p, ParamID::tempo, 150.0f);
    s01 (p, ParamID::seqTarget, 0.0f);               // P1
    s01 (p, ParamID::seqOn, 1.0f);

    juce::AudioBuffer<float> buf (2, 128); juce::MidiBuffer m;
    for (int b = 0; b < 100; ++b) { buf.clear(); p.processBlock (buf, m); }
    s01 (p, ParamID::seqTarget, 1.0f / 3.0f);        // switch target to P2 mid-gate
    for (int b = 0; b < 100; ++b) { buf.clear(); p.processBlock (buf, m); }
    s01 (p, ParamID::seqOn, 0.0f);                   // stop everything
    REQUIRE (tailPeak (p) < 1.0e-4f);                // no voice left hanging on P1
}
