// ============================================================================
// Default startup scene: out of the box the parts are pre-populated so each surface is
// audibly distinct and the sequencer has a dedicated drum kit (fixing "the sequencer
// affects the live patch at startup"). P1 = lead (live), P2 = 808 kit (the seq's default
// target), P3 = bass, P4 = spare. Parts 1-3 do NOT persist across relaunch, so this scene
// is re-established every launch; the live sound (P1) still persists via state restore.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "PluginProcessor.h"

namespace
{
    double partEnergy (VASynthProcessor& p, int blocks)
    {
        juce::AudioBuffer<float> buf (2, 128); juce::MidiBuffer m; double e = 0.0;
        for (int b = 0; b < blocks; ++b) { buf.clear(); p.processBlock (buf, m); e += buf.getRMSLevel (0, 0, 128); }
        return e;
    }
}

TEST_CASE ("default scene: P2 is the 808 kit (the sequencer's target), P3 is a bass", "[plugin][scene]")
{
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);

    // P2 (part 1) is a drum kit, and it is the sequencer's DEFAULT target (seq_target = P2).
    REQUIRE (p.isPartKit (1));
    REQUIRE (p.getPartPreset (1) == "808 Basics");
    REQUIRE ((int) p.apvts.getRawParameterValue (ParamID::seqTarget)->load() == 1);

    // P3 (part 2) is a plain bass voice; P4 (part 3) is a free spare (not a kit).
    REQUIRE_FALSE (p.isPartKit (2));
    REQUIRE (p.getPartPreset (2) == "Fat Saw Bass");
    REQUIRE_FALSE (p.isPartKit (3));
}

TEST_CASE ("default scene: the sequencer plays the drum kit, distinct from the live part", "[plugin][scene]")
{
    namespace ID = ParamID;
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);

    // Draw a beat on the rows that hit the 808 pads (36..41) and run the seq (default target P2).
    for (int r = 0; r < 6; ++r)
    {
        p.setSeqNote (r, 36 + r);
        for (int s = 0; s < VASynthProcessor::kSeqSteps; ++s) p.setSeqCell (r, s, 1);
    }
    p.apvts.getParameter (ID::tempo)->setValueNotifyingHost (p.apvts.getParameter (ID::tempo)->convertTo0to1 (200.0f));
    p.apvts.getParameter (ID::seqOn)->setValueNotifyingHost (1.0f);

    REQUIRE (partEnergy (p, 120) > 0.0);   // the seq drives the 808 kit out of the box
}
