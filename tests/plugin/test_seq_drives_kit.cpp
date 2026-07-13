// ============================================================================
// Regression: a sequencer whose target part holds a drum KIT drives that kit — assigning
// a kit to the seq's target part does NOT "kill" the sequencer. (User reported "switching
// to the 808 kit kills the sequencer"; the mechanism is sound — the real gap is that the
// seq and a kit are not wired to the same part by DEFAULT, a scene-setup choice.)
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
}

TEST_CASE ("sequencer targeting an 808 kit part drives the kit", "[plugin][seq][kit]")
{
    namespace ID = ParamID;
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);

    // Assign the 808 kit to part 1 (P2 = the seq's DEFAULT target).
    p.setPartKit (1, p.factoryKit ("808 Basics"));

    // Draw a pattern on the rows whose notes hit the kit pads (36..41).
    for (int r = 0; r < 6; ++r) { p.setSeqNote (r, 36 + r); for (int s = 0; s < VASynthProcessor::kSeqSteps; ++s) p.setSeqCell (r, s, 1); }
    setVal (p, ID::seqTarget, 1.0f);   // choice index 1 = "P2" = part 1 (the kit part)
    setVal (p, ID::tempo, 200.0f);
    s01 (p, ID::seqOn, 1.0f);

    const double e = energy (p, 120);   // run a few loops
    INFO ("seq->kit energy = " << e);
    REQUIRE (e > 0.0);                   // the kit sounds under the sequencer (not "killed")
}
