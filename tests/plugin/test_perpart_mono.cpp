// ============================================================================
// Per-part Poly/Mono/Legato (task #57). Mono/Legato used to be a single GLOBAL mode that
// shared one voice across all parts, so a mono lead on part 1 was cut by the sequencer on
// part 4. Now each part has its own mode + mono voice + note stack. These lock that in.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "PluginProcessor.h"

namespace
{
    void s01 (VASynthProcessor& p, const char* id, float v) { p.apvts.getParameter (id)->setValueNotifyingHost (v); }
    void setVal (VASynthProcessor& p, const char* id, float v)
    { p.apvts.getParameter (id)->setValueNotifyingHost (p.apvts.getParameter (id)->convertTo0to1 (v)); }
    void run (VASynthProcessor& p, int blocks)
    { juce::AudioBuffer<float> b (2, 128); juce::MidiBuffer m; for (int i = 0; i < blocks; ++i) { b.clear(); p.processBlock (b, m); } }

    void startSeqOnKit (VASynthProcessor& p)   // default scene: part 4 = 808 kit, seq target = P4
    {
        for (int r = 0; r < 6; ++r) { p.setSeqNote (r, 36 + r); for (int s = 0; s < VASynthProcessor::kSeqSteps; ++s) p.setSeqCell (r, s, 1); }
        setVal (p, ParamID::tempo, 220.0f);
        s01 (p, ParamID::seqOn, 1.0f);
    }

    // Force a part to a given poly mode (0 poly / 1 mono / 2 legato) via the edit-focus path.
    void setPartMode (VASynthProcessor& p, int part, float mode)
    {
        p.setEditFocus (part);
        setVal (p, ParamID::polyMode, mode);
        p.setEditFocus (0);
    }
}

TEST_CASE ("per-part mono: a held MONO note on part 1 survives the sequencer on part 4", "[plugin][voices][isolation][mono]")
{
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    p.setPartPreset (1, "Fat Saw Bass");     // a MONO patch (poly_mode:1), baked onto part 1
    startSeqOnKit (p);
    run (p, 20);

    p.routeNoteOn (67, 0.9f, 1);             // hold a continuous note on the mono part
    run (p, 4);
    REQUIRE (p.activeVoicesForPart (1) >= 1);
    run (p, 200);                            // the sequencer chokes/retriggers part 4 many times
    REQUIRE (p.activeVoicesForPart (1) >= 1);   // ...the mono part-1 note is untouched
}

TEST_CASE ("per-part mono: a MONO part and a POLY part play at the same time", "[plugin][voices][isolation][mono]")
{
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    setPartMode (p, 1, 1.0f);                // part 1 = MONO
    setPartMode (p, 2, 0.0f);               // part 2 = POLY

    p.routeNoteOn (60, 0.9f, 1);             // one note on the mono part
    for (int n : { 48, 52, 55, 59 }) p.routeNoteOn (n, 0.9f, 2);   // a 4-note chord on the poly part
    run (p, 4);

    REQUIRE (p.activeVoicesForPart (1) == 1);   // mono part: exactly one voice
    REQUIRE (p.activeVoicesForPart (2) == 4);   // poly part: all four, independently
}

TEST_CASE ("per-part mono: a KIT part stays POLY even when Mono is selected", "[plugin][voices][isolation][mono][kit]")
{
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    p.setPartKit (1, p.factoryKit ("808 Basics"));   // kit on part 1
    setPartMode (p, 1, 1.0f);                        // ...and select Mono for it

    for (int n : { 36, 37, 38, 39, 40 }) p.routeNoteOn (n, 0.9f, 1);   // 5 simultaneous pads
    run (p, 4);
    REQUIRE (p.activeVoicesForPart (1) >= 4);        // kit ignored Mono -> polyphonic drums
}

TEST_CASE ("per-part mono: switching a part's mode mid-note leaves nothing stuck", "[plugin][voices][isolation][mono]")
{
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    setPartMode (p, 1, 1.0f);                // mono
    p.routeNoteOn (64, 0.9f, 1);
    run (p, 8);
    REQUIRE (p.activeVoicesForPart (1) == 1);

    setPartMode (p, 1, 0.0f);                // switch to poly mid-note (releases the part's voices)
    run (p, 160);                            // let the released voice's envelope finish (~0.15 s release)
    REQUIRE (p.activeVoicesForPart (1) == 0);   // released cleanly, no stuck note
}
