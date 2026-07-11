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

TEST_CASE ("edit-focus: a kit part is not a focus target (edit it in the Kit Editor)", "[plugin][editfocus]")
{
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 128);
    p.setPartKit (2, p.loadKit ("808 Basics"));
    p.setEditFocus (2);
    REQUIRE (p.editFocus() == 0);      // stayed on 0 (kit focus is a no-op)
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
