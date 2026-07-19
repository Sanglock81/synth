// ============================================================================
// J2 — per-part looper loop lengths. Each lane's loop_bars param drives ITS OWN length
// (a shorter lane's playhead advances proportionally faster), and the append-only enum
// extension {1,2,4,8,16,32} restores old sessions (that only had loop_bars) unchanged.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginProcessor.h"

namespace
{
    int choiceIndex (VASynthProcessor& p, const char* id)
    {
        return dynamic_cast<juce::AudioParameterChoice*> (p.apvts.getParameter (id))->getIndex();
    }
    void setChoice (VASynthProcessor& p, const char* id, int index)
    {
        auto* cp = p.apvts.getParameter (id);
        cp->setValueNotifyingHost (cp->convertTo0to1 ((float) index));
    }
}

TEST_CASE ("J2 migration: an old session (only loop_bars) restores unchanged; new lanes default", "[plugin][looper][j2][migration]")
{
    VASynthProcessor a; a.prepareToPlay (48000.0, 128);
    setChoice (a, ParamID::loopBars, 2);          // old sessions could store 0/1/2 == 1/2/4 bars
    REQUIRE (choiceIndex (a, ParamID::loopBars) == 2);

    // Snapshot, then STRIP the J2-added params to simulate a pre-J2 session that never had them.
    auto state = a.apvts.copyState();
    for (int i = state.getNumChildren() - 1; i >= 0; --i)
    {
        auto child = state.getChild (i);
        if (child.hasType ("PARAM"))
        {
            const auto id = child.getProperty ("id").toString();
            if (id == ParamID::loopBars2 || id == ParamID::loopBars3 || id == ParamID::loopBars4)
                state.removeChild (i, nullptr);
        }
    }

    VASynthProcessor b; b.prepareToPlay (48000.0, 128);
    b.apvts.replaceState (state);
    REQUIRE (choiceIndex (b, ParamID::loopBars)  == 2);   // index 2 still means "4 bars" (append-only)
    REQUIRE (choiceIndex (b, ParamID::loopBars2) == 0);   // absent -> defaults to 1 bar
    REQUIRE (choiceIndex (b, ParamID::loopBars3) == 0);
    REQUIRE (choiceIndex (b, ParamID::loopBars4) == 0);
}

TEST_CASE ("J2: each lane's length follows its OWN bars param", "[plugin][looper][j2]")
{
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    p.apvts.getParameter (ParamID::tempo)->setValueNotifyingHost (
        p.apvts.getParameter (ParamID::tempo)->convertTo0to1 (120.0f));   // bar = 96000 samples @48k
    setChoice (p, ParamID::loopBars,  0);         // lane 0 (P1) = 1 bar
    setChoice (p, ParamID::loopBars2, 1);         // lane 1 (P2) = 2 bars

    // Run half of lane 0's bar (48000 samples = 375 blocks). Lane 0 is half done; lane 1, being
    // twice as long, is a quarter done — the playheads are in exact 2:1 proportion.
    juce::AudioBuffer<float> buf (2, 128); juce::MidiBuffer m;
    for (int b = 0; b < 375; ++b) { buf.clear(); m.clear(); p.processBlock (buf, m); }

    const float ph0 = p.loopPlayhead (0);
    const float ph1 = p.loopPlayhead (1);
    REQUIRE (ph0 == Catch::Approx (0.5).margin (0.02));
    REQUIRE (ph1 == Catch::Approx (0.25).margin (0.02));
    REQUIRE (ph0 == Catch::Approx (2.0 * ph1).margin (0.02));   // lane 0 is exactly half as long
}

TEST_CASE ("J2: the 32-bar length is selectable and lengthens the loop 32x vs 1 bar", "[plugin][looper][j2]")
{
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    p.apvts.getParameter (ParamID::tempo)->setValueNotifyingHost (
        p.apvts.getParameter (ParamID::tempo)->convertTo0to1 (120.0f));
    setChoice (p, ParamID::loopBars,  0);         // lane 0 = 1 bar  (96000 samples)
    setChoice (p, ParamID::loopBars2, 5);         // lane 1 = 32 bars

    // One bar of processing: lane 0 completes exactly one loop (playhead wraps back near 0),
    // lane 1 is 1/32 of the way through.
    juce::AudioBuffer<float> buf (2, 128); juce::MidiBuffer m;
    for (int b = 0; b < 750; ++b) { buf.clear(); m.clear(); p.processBlock (buf, m); }   // 96000 samples
    REQUIRE (p.loopPlayhead (1) == Catch::Approx (1.0 / 32.0).margin (0.01));             // 1/32 through
    REQUIRE (p.loopPlayhead (0) < 0.05f);                                                  // lane 0 wrapped
}
