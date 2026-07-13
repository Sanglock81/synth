// ============================================================================
// Edit focus (1.3): tapping a part swaps the panel to THAT part's sound; edits
// stick per part; global/performance params stay put; focus 0 is the default.
// Plus click-torture over rapid focus swaps while playing (noise-cleanliness rule).
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginProcessor.h"
#include <cmath>

namespace
{
    float pv (VASynthProcessor& p, const char* id) { return p.apvts.getRawParameterValue (id)->load(); }
    void  set01 (VASynthProcessor& p, const char* id, float v) { p.apvts.getParameter (id)->setValueNotifyingHost (v); }
}

TEST_CASE ("edit-focus: panel swaps to the part's sound; per-part edits persist", "[plugin][editfocus]")
{
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 128);
    p.setPartPreset (1, "Init");   // part 1 = plain synth (the default scene makes it a kit, which the panel can't focus)
    REQUIRE (p.editFocus() == 0);

    // Part 0 (LIVE): dial in a distinctive cutoff, and a GLOBAL tempo.
    set01 (p, ParamID::filterCutoff, 0.30f);
    const float A = pv (p, ParamID::filterCutoff);
    set01 (p, ParamID::tempo, 0.80f);
    const float tempoSet = pv (p, ParamID::tempo);
    // a GLOBAL mixer level too
    set01 (p, ParamID::part2Level, 0.75f);
    const float mixSet = pv (p, ParamID::part2Level);

    // Focus part 1 -> the panel (APVTS) swaps to part 1's sound.
    p.setEditFocus (1);
    REQUIRE (p.editFocus() == 1);
    set01 (p, ParamID::filterCutoff, 0.90f);
    const float B = pv (p, ParamID::filterCutoff);
    REQUIRE (B != Catch::Approx (A));

    // Global params were NOT swapped by the focus change.
    REQUIRE (pv (p, ParamID::tempo)      == Catch::Approx (tempoSet).margin (1e-5));
    REQUIRE (pv (p, ParamID::part2Level) == Catch::Approx (mixSet).margin (1e-5));

    // Tap back to part 0 -> its cutoff is restored.
    p.setEditFocus (0);
    REQUIRE (pv (p, ParamID::filterCutoff) == Catch::Approx (A).margin (1e-4));

    // Tap part 1 again -> the edit stuck.
    p.setEditFocus (1);
    REQUIRE (pv (p, ParamID::filterCutoff) == Catch::Approx (B).margin (1e-4));

    // Globals still put after all the swapping.
    REQUIRE (pv (p, ParamID::tempo)      == Catch::Approx (tempoSet).margin (1e-5));
    REQUIRE (pv (p, ParamID::part2Level) == Catch::Approx (mixSet).margin (1e-5));
}

TEST_CASE ("edit-focus: MULTI captures + restores an edited part's custom sound", "[plugin][editfocus][multi]")
{
    VASynthProcessor src;
    src.prepareToPlay (48000.0, 128);
    src.setPartPreset (1, "Init");            // part 1 = a clean preset
    src.setEditFocus (1);
    set01 (src, ParamID::filterCutoff, 0.77f);   // edit it on the panel
    const float edited = pv (src, ParamID::filterCutoff);
    REQUIRE (src.partIsEdited (1));

    const auto multi = src.captureMultiState();

    VASynthProcessor dst;
    dst.prepareToPlay (48000.0, 128);
    dst.applyMultiState (multi);
    REQUIRE (dst.partIsEdited (1));           // recalled as edited, not the clean preset
    dst.setEditFocus (1);
    REQUIRE (pv (dst, ParamID::filterCutoff) == Catch::Approx (edited).margin (1e-3));
}

TEST_CASE ("edit-focus: revert restores the clean preset (clears edited)", "[plugin][editfocus]")
{
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 128);
    p.setPartPreset (1, "Init");
    const float clean = pv (p, ParamID::filterCutoff);   // part 0 apvts still Init-ish, but Init cutoff
    p.setEditFocus (1);
    const float preset = pv (p, ParamID::filterCutoff);
    set01 (p, ParamID::filterCutoff, 0.15f);
    REQUIRE (p.partIsEdited (1));
    p.revertPartToPreset (1);
    REQUIRE_FALSE (p.partIsEdited (1));
    REQUIRE (pv (p, ParamID::filterCutoff) == Catch::Approx (preset).margin (1e-3));
    juce::ignoreUnused (clean);
}

TEST_CASE ("edit-focus: tapping a kit part plays it but keeps the panel on the synth", "[plugin][editfocus]")
{
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 128);
    p.setPartKit (2, p.loadKit ("808 Basics"));
    p.setEditFocus (2);
    REQUIRE (p.playFocus() == 2);      // the keyboard now plays the kit (per-pad)
    REQUIRE (p.editFocus() == 0);      // panel stays editing the synth (edit the kit in the Kit Editor)
}

TEST_CASE ("edit-focus: playing a loaded kit triggers per-pad drums, not one pitched voice", "[plugin][editfocus][kit]")
{
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 128);
    p.setPartKit (2, p.loadKit ("808 Basics"));   // pads on trigger notes 36..41
    p.setEditFocus (2);                            // route the keyboard to the kit

    auto rms = [&] (int trigNote, int blocks)
    {
        juce::AudioBuffer<float> buf (2, 128);
        p.routeNoteOn (trigNote, 1.0f, 0);         // LIVE note -> play-focus (the kit)
        double e = 0.0;
        for (int b = 0; b < blocks; ++b) { buf.clear(); juce::MidiBuffer m; p.processBlock (buf, m); e += buf.getRMSLevel (0, 0, 128); }
        p.routeNoteOff (trigNote, 0);
        return e;
    };
    // Two different kit trigger notes both make sound (different pads), proving the kit is
    // being played per-pad rather than a single pitched synth voice on the wrong part.
    REQUIRE (rms (36, 12) > 0.0);      // kick pad
    REQUIRE (rms (38, 12) > 0.0);      // snare pad
}

TEST_CASE ("edit-focus: switching parts while holding a note releases it (no stuck note)", "[plugin][editfocus]")
{
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 128);
    set01 (p, ParamID::ampRelease, 0.0f);      // fast release so a freed note decays quickly

    auto energy = [&] (int blocks)
    {
        juce::AudioBuffer<float> buf (2, 128); juce::MidiBuffer m; double e = 0.0;
        for (int b = 0; b < blocks; ++b) { buf.clear(); p.processBlock (buf, m); e += buf.getRMSLevel (0, 0, 128); }
        return e;
    };

    p.routeNoteOn (60, 0.9f, 0);               // play a held note on part 0 (focus 0)
    REQUIRE (energy (8) > 0.0);                // it's sounding

    p.setEditFocus (1);                        // switch parts WITHOUT releasing the key
    energy (4);                                // let the hand-off release it
    REQUIRE (energy (40) < 1.0e-3);            // the held note was released, not stuck
}

TEST_CASE ("torture: rapid edit-focus swaps while playing stay click-free", "[plugin][click][editfocus]")
{
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 128);
    // Give parts 1-3 distinct sounds so swaps change the live timbre.
    for (int part = 1; part <= 3; ++part) set01 (p, ParamID::filterCutoff, 0.2f + 0.2f * part);  // (edits whatever's focused; parts start Init)

    juce::AudioBuffer<float> buf (2, 128);
    juce::MidiBuffer midi;
    float prevL = 0, prevR = 0, maxJump = 0, peak = 0; bool finite = true;
    auto pump = [&] (int blocks)
    {
        for (int b = 0; b < blocks; ++b)
        {
            buf.clear(); juce::MidiBuffer empty; p.processBlock (buf, empty);
            const float* L = buf.getReadPointer (0); const float* R = buf.getReadPointer (1);
            for (int i = 0; i < 128; ++i)
            {
                finite = finite && std::isfinite (L[i]) && std::isfinite (R[i]);
                peak = std::max ({ peak, std::abs (L[i]), std::abs (R[i]) });
                maxJump = std::max ({ maxJump, std::abs (L[i] - prevL), std::abs (R[i] - prevR) });
                prevL = L[i]; prevR = R[i];
            }
        }
    };

    p.routeNoteOn (60, 0.9f, 0);       // hold a note on the LIVE (focused) part
    for (int cycle = 0; cycle < 24; ++cycle)
    {
        p.setEditFocus (cycle % 4);    // swap the focused/live part under the held note
        pump (12);
    }
    INFO ("peak=" << peak << " maxJump=" << maxJump);
    REQUIRE (finite);
    REQUIRE (peak <= 1.0f);
    REQUIRE (maxJump < 0.35f);
}
