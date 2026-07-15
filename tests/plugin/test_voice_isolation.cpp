// ============================================================================
// Per-part voice isolation: a generator (sequencer/arp) on one part must never cut
// or starve live playing on another part. Generator voices yield to live-played
// voices, so the sequencer can't steal the lead's notes.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "PluginProcessor.h"

namespace
{
    void s01 (VASynthProcessor& p, const char* id, float v) { p.apvts.getParameter (id)->setValueNotifyingHost (v); }
    void setVal (VASynthProcessor& p, const char* id, float v)
    { p.apvts.getParameter (id)->setValueNotifyingHost (p.apvts.getParameter (id)->convertTo0to1 (v)); }

    double energy (VASynthProcessor& p, int blocks)
    {
        juce::AudioBuffer<float> buf (2, 128); juce::MidiBuffer m; double e = 0.0;
        for (int b = 0; b < blocks; ++b) { buf.clear(); p.processBlock (buf, m); e += buf.getRMSLevel (0, 0, 128); }
        return e;
    }

    void setSeqDense (VASynthProcessor& p, int targetPart)
    {
        for (int r = 0; r < VASynthProcessor::kSeqRows; ++r)
        {
            p.setSeqNote (r, 40 + r);
            for (int s = 0; s < VASynthProcessor::kSeqSteps; ++s) p.setSeqCell (r, s, 1);   // every row/step
        }
        s01 (p, ParamID::seqTarget, (float) targetPart / 3.0f);
        setVal (p, ParamID::seqGate, 0.98f);       // long gates -> notes overlap and hold voices
        setVal (p, ParamID::tempo, 260.0f);
        s01 (p, ParamID::seqOn, 1.0f);
    }
}

TEST_CASE ("voice isolation: a live lead gets all its voices while a sequencer runs on another part", "[plugin][voices][isolation]")
{
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    // Fat Saw Bass is a MONO patch; this test is about POLY voice isolation, so force both
    // parts poly (edit-focus each, set Poly, focus away to bake). Part 2 must be poly so the
    // sequencer actually fills the pool with generator voices.
    auto makePoly = [&] (int part)
    {
        p.setPartPreset (part, "Fat Saw Bass");
        p.setEditFocus (part);
        p.apvts.getParameter (ParamID::polyMode)->setValueNotifyingHost (0.0f);   // Poly
        p.setEditFocus (0);
    };
    makePoly (1);
    makePoly (2);
    setSeqDense (p, 2);                             // dense sequencer hammering part 2 (P3)
    energy (p, 80);                                // let the generator fill the pool

    // Play a 6-note lead on part 1.
    for (int n : { 60, 64, 67, 71, 74, 79 }) p.routeNoteOn (n, 0.9f, 1);
    energy (p, 4);                                 // let them allocate

    // The lead got ALL 6 of its voices — the sequencer's generator voices yielded.
    REQUIRE (p.activeVoicesForPart (1) == 6);
}
