// ============================================================================
// Load isolation: loading a patch (factory / user / Init) into the focused part must
// change ONLY that part's sound. All global performance state — the sequencer pattern +
// target + on, tempo, macros — and every OTHER part stay exactly as they were, so a load
// never interrupts what is currently playing. (User: "I would like to be able to load
// patches without interrupting the system.")
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginProcessor.h"

namespace
{
    float raw (VASynthProcessor& p, const char* id) { return p.apvts.getRawParameterValue (id)->load(); }
    void  setVal (VASynthProcessor& p, const char* id, float v)
    { p.apvts.getParameter (id)->setValueNotifyingHost (p.apvts.getParameter (id)->convertTo0to1 (v)); }
    void  set01 (VASynthProcessor& p, const char* id, float v) { p.apvts.getParameter (id)->setValueNotifyingHost (v); }

    double partEnergy (VASynthProcessor& p, int blocks)
    {
        juce::AudioBuffer<float> buf (2, 128); juce::MidiBuffer m; double e = 0.0;
        for (int b = 0; b < blocks; ++b) { buf.clear(); p.processBlock (buf, m); e += buf.getRMSLevel (0, 0, 128); }
        return e;
    }

    // Set up a distinctive global performance state (sequencer + tempo + a macro).
    void primeGlobals (VASynthProcessor& p)
    {
        namespace ID = ParamID;
        set01 (p, ID::seqOn, 1.0f);
        setVal (p, ID::seqTarget, 2.0f);      // P3
        setVal (p, ID::tempo, 141.0f);
        setVal (p, ID::macro1, 0.73f);
        for (int r = 0; r < VASynthProcessor::kSeqRows; ++r)
        {
            p.setSeqNote (r, 40 + r);
            for (int s = 0; s < VASynthProcessor::kSeqSteps; ++s) p.setSeqCell (r, s, (r + s) % 3 == 0 ? 1 : 0);
        }
    }
}

TEST_CASE ("load isolation: loading a factory preset leaves the sequencer + globals untouched", "[plugin][load][isolation]")
{
    namespace ID = ParamID;
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    primeGlobals (p);

    // Snapshot the globals + a couple of seq cells.
    const float  seqOn0  = raw (p, ID::seqOn);
    const float  seqTgt0 = raw (p, ID::seqTarget);
    const float  tempo0  = raw (p, ID::tempo);
    const float  macro0  = raw (p, ID::macro1);
    const int    cellA   = p.getSeqCell (0, 0);
    const int    cellB   = p.getSeqCell (3, 6);
    const float  cutoff0 = raw (p, ID::filterCutoff);

    p.loadFactoryPreset ("Fat Saw Bass");

    // Every global is exactly where it was.
    REQUIRE (raw (p, ID::seqOn)     == seqOn0);
    REQUIRE (raw (p, ID::seqTarget) == seqTgt0);
    REQUIRE (raw (p, ID::tempo)     == Catch::Approx (tempo0));
    REQUIRE (raw (p, ID::macro1)    == Catch::Approx (macro0));
    REQUIRE (p.getSeqCell (0, 0)    == cellA);
    REQUIRE (p.getSeqCell (3, 6)    == cellB);

    // ...but the focused part's SOUND actually changed (the preset moved a sound param).
    REQUIRE (raw (p, ID::filterCutoff) != Catch::Approx (cutoff0));
}

TEST_CASE ("load isolation: Init resets only the focused sound, keeping the sequencer running", "[plugin][load][isolation]")
{
    namespace ID = ParamID;
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    primeGlobals (p);
    // Move the focused sound well away from Init so the reset is observable.
    setVal (p, ID::filterCutoff, 320.0f);
    const float moved = raw (p, ID::filterCutoff);

    p.loadInitPreset();

    REQUIRE (raw (p, ID::seqOn)     == Catch::Approx (1.0f));   // sequencer still on
    REQUIRE (raw (p, ID::seqTarget) == Catch::Approx (2.0f));   // still targeting P3
    REQUIRE (raw (p, ID::tempo)     == Catch::Approx (141.0f)); // tempo untouched
    REQUIRE (p.getSeqCell (0, 0)    == 1);                      // pattern intact
    REQUIRE (raw (p, ID::filterCutoff) != Catch::Approx (moved));   // sound WAS reset
}

TEST_CASE ("load isolation: loading a preset on the live part keeps another part playing", "[plugin][load][isolation]")
{
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    // Part 1 (P2) gets a sound and is left ringing.
    p.setPartPreset (1, "Fat Saw Bass");
    for (int n : { 48, 55 }) p.routeNoteOn (n, 0.9f, 1);
    (void) partEnergy (p, 4);
    const int voicesBefore = p.activeVoicesForPart (1);
    REQUIRE (voicesBefore > 0);

    // Load a different sound onto the focused LIVE part (part 0). Part 1's baked voices
    // must be untouched — the load doesn't reset or steal them.
    p.loadFactoryPreset ("Fat Saw Bass");

    REQUIRE (p.activeVoicesForPart (1) == voicesBefore);       // other part's notes survive
    REQUIRE (partEnergy (p, 4) > 0.0);                          // ...and still make sound
}
