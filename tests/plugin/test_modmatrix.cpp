// ============================================================================
// Mod matrix (#56) at the plugin layer: per-part routes persist with the patch, the
// LINK helper fills/reuses/limits slots, and a live route audibly modulates through
// processBlock end-to-end.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginProcessor.h"
#include "UI/ModMatrixPanel.h"
#include "UI/Widgets.h"

TEST_CASE ("mod matrix routes persist across a state round-trip (#56)", "[plugin][modmatrix][state]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor src;
    src.setModSlot (0, 0, ModMatrix::LFO2,     ModMatrix::Cutoff, 0.5f);
    src.setModSlot (1, 3, ModMatrix::Velocity, ModMatrix::Amp,   -0.75f);

    juce::MemoryBlock blob; src.getStateInformation (blob);
    VASynthProcessor dst; dst.setStateInformation (blob.getData(), (int) blob.getSize());

    const auto s0 = dst.getModSlot (0, 0);
    REQUIRE (s0.source == ModMatrix::LFO2);
    REQUIRE (s0.dest   == ModMatrix::Cutoff);
    REQUIRE (s0.depth  == Catch::Approx (0.5f).margin (1e-3));

    const auto s1 = dst.getModSlot (1, 3);
    REQUIRE (s1.source == ModMatrix::Velocity);
    REQUIRE (s1.dest   == ModMatrix::Amp);
    REQUIRE (s1.depth  == Catch::Approx (-0.75f).margin (1e-3));
}

TEST_CASE ("linkModRoute fills free slots, reuses a pair, and reports full (#56)", "[plugin][modmatrix]")
{
    VASynthProcessor p;

    REQUIRE (p.linkModRoute (0, ModMatrix::LFO1, ModMatrix::Cutoff, 0.5f) == 0);
    REQUIRE (p.linkModRoute (0, ModMatrix::LFO1, ModMatrix::Cutoff, 0.8f) == 0);   // same pair -> reuse
    REQUIRE (p.getModSlot (0, 0).depth == Catch::Approx (0.8f).margin (1e-3));      // ...updates the depth
    REQUIRE (p.linkModRoute (0, ModMatrix::Velocity, ModMatrix::Amp) == 1);         // new pair -> next free

    VASynthProcessor q;
    for (int i = 0; i < ModMatrix::kSlots; ++i)                                     // 8 distinct pairs fill it
        REQUIRE (q.linkModRoute (0, ModMatrix::Macro1 + i, ModMatrix::Cutoff) == i);
    REQUIRE (q.linkModRoute (0, ModMatrix::LFO1, ModMatrix::Pitch) == -1);          // full
}

namespace
{
    // Peak of the live part over a short render, with `mw` mod wheel.
    float blockPeak (VASynthProcessor& p, float mw)
    {
        p.prepareToPlay (48000.0, 128);
        float peak = 0.0f;
        for (int b = 0; b < 24; ++b)
        {
            juce::AudioBuffer<float> buf (2, 128); buf.clear();
            juce::MidiBuffer midi;
            if (b == 0) { midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
                          midi.addEvent (juce::MidiMessage::controllerEvent (1, 1, (int) (mw * 127)), 0); }
            p.processBlock (buf, midi);
            if (b >= 6) peak = std::max (peak, buf.getMagnitude (0, 128));
        }
        return peak;
    }
}

TEST_CASE ("a live ModWheel->Amp route modulates the sound through processBlock (#56)", "[plugin][modmatrix]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    VASynthProcessor off; const float base = blockPeak (off, 1.0f);   // no route: wheel only vibratos

    VASynthProcessor on;
    on.linkModRoute (0, ModMatrix::ModWheel, ModMatrix::Amp, 1.0f);    // wheel -> amp on the focused part
    const float lifted = blockPeak (on, 1.0f);

    REQUIRE (base > 0.0f);
    REQUIRE (lifted > base * 1.4f);
}

TEST_CASE ("mod matrix overlay mirrors and edits the focused part (#56)", "[plugin][modmatrix][ui]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    p.setModSlot (-1, 0, ModMatrix::LFO2, ModMatrix::Cutoff, 0.5f);   // a route on the focused part

    ModMatrixPanel panel (p);                                         // ctor refreshes from the processor
    REQUIRE (panel.rowSource (0) == ModMatrix::LFO2);
    REQUIRE (panel.rowDest   (0) == ModMatrix::Cutoff);
    REQUIRE (panel.rowDepth  (0) == Catch::Approx (0.5f).margin (1e-2));

    // Build a route from the dropdowns + depth, through the real change handlers.
    panel.pickForTest (1, ModMatrix::Velocity, ModMatrix::Amp, -0.8f);
    const auto s = p.getModSlot (-1, 1);
    REQUIRE (s.source == ModMatrix::Velocity);
    REQUIRE (s.dest   == ModMatrix::Amp);
    REQUIRE (s.depth  == Catch::Approx (-0.8f).margin (1e-2));
}

TEST_CASE ("a destination knob is armable only while a source is armed, and completing routes it (#56)",
           "[plugin][modmatrix][ui]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;

    RotaryKnob knob (p.apvts, ParamID::filterCutoff, "CUTOFF", p.getMidiLearn());
    knob.setModDestination (p, ModMatrix::Cutoff);
    REQUIRE_FALSE (knob.isLinkArmable());          // nothing armed yet

    p.armModLink (ModMatrix::LFO1);
    REQUIRE (knob.isLinkArmable());                // source armed -> this dest lights up
    REQUIRE (p.linkArmed());

    // The knob's press completes through the same ModLinkController path the LinkSlider uses.
    const int slot = p.completeModLink (ModMatrix::Cutoff);
    REQUIRE (slot >= 0);
    REQUIRE_FALSE (p.linkArmed());                 // completing disarms
    REQUIRE_FALSE (knob.isLinkArmable());

    const auto s = p.getModSlot (-1, slot);
    REQUIRE (s.source == ModMatrix::LFO1);
    REQUIRE (s.dest   == ModMatrix::Cutoff);
    REQUIRE (s.depth  > 0.0f);                      // a usable default depth, tunable by drag

    // Re-grab depth within the window mirrors setModRouteDepth on that slot.
    p.setModRouteDepth (slot, -0.6f);
    REQUIRE (p.modRouteDepth (slot) == Catch::Approx (-0.6f).margin (1e-3));
}
