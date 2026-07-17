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

namespace
{
    // Late-tail peak after a short note, with the delay FX on and Macro 4 at `macro4`.
    float delayTail (VASynthProcessor& p, float macro4)
    {
        p.prepareToPlay (48000.0, 128);
        p.apvts.getParameter (ParamID::fxDelayOn)->setValueNotifyingHost (1.0f);
        p.apvts.getParameter (ParamID::delayMix)->setValueNotifyingHost (0.8f);
        p.apvts.getParameter (ParamID::delayTime)->setValueNotifyingHost (0.15f);   // short repeats
        p.apvts.getParameter (ParamID::delayFeedback)->setValueNotifyingHost (0.1f);// low base
        p.apvts.getParameter (ParamID::macro4)->setValueNotifyingHost (macro4);
        float tail = 0.0f;
        for (int b = 0; b < 220; ++b)
        {
            juce::AudioBuffer<float> buf (2, 128); buf.clear();
            juce::MidiBuffer midi;
            if (b == 0) midi.addEvent (juce::MidiMessage::noteOn  (1, 60, 0.9f), 0);
            if (b == 6) midi.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
            p.processBlock (buf, midi);
            if (b >= 130) tail = std::max (tail, buf.getMagnitude (0, 128));   // long after note-off
        }
        return tail;
    }
}

TEST_CASE ("block-tier: Macro -> delay feedback modulates the FX at block rate (#56 G4)",
           "[plugin][modmatrix][blocktier]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // Same route in both; only Macro 4 differs. High macro pushes feedback up -> the delay
    // tail rings on far longer, so the late-window energy is much larger.
    VASynthProcessor lo; lo.setModSlot (-1, 0, ModMatrix::Macro4, ModMatrix::DelayFeedback, 1.0f);
    VASynthProcessor hi; hi.setModSlot (-1, 0, ModMatrix::Macro4, ModMatrix::DelayFeedback, 1.0f);

    const float tLo = delayTail (lo, 0.0f);
    const float tHi = delayTail (hi, 1.0f);

    REQUIRE (tLo >= 0.0f);
    REQUIRE (tHi > tLo * 2.0f + 1.0e-4f);     // the macro audibly lengthens the delay tail
}

TEST_CASE ("block-tier: an LFO drives an FX dest and the offset varies over time (#56 G4)",
           "[plugin][modmatrix][blocktier]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    p.setModSlot (-1, 0, ModMatrix::LFO1, ModMatrix::ReverbMix, 1.0f);   // LFO1 -> reverb mix
    p.apvts.getParameter (ParamID::lfoRate)->setValueNotifyingHost (0.6f);   // audible LFO rate
    p.apvts.getParameter (ParamID::lfoDepth)->setValueNotifyingHost (0.5f);
    p.prepareToPlay (48000.0, 128);

    // Hold a note so the focused part has voices (LFOs only advance for sounding parts), and
    // watch the published block offset for ReverbMix swing as the LFO cycles.
    float lo = 1.0e9f, hi = -1.0e9f;
    for (int b = 0; b < 300; ++b)
    {
        juce::AudioBuffer<float> buf (2, 128); buf.clear();
        juce::MidiBuffer midi;
        if (b == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
        p.processBlock (buf, midi);
        const float o = p.blockModOffset (ModMatrix::ReverbMix);
        lo = std::min (lo, o); hi = std::max (hi, o);
    }
    REQUIRE (hi - lo > 0.2f);        // the LFO actually sweeps the reverb-mix offset
}

TEST_CASE ("block-tier is inert with no block route (bit-identical) (#56 G4)", "[plugin][modmatrix][blocktier]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    // A voice-tier-only route must leave the block-tier a no-op: delay tail identical with the
    // macro low vs high (nothing routes to a block dest).
    VASynthProcessor a; a.setModSlot (-1, 0, ModMatrix::LFO1, ModMatrix::Cutoff, 0.5f);
    VASynthProcessor b; b.setModSlot (-1, 0, ModMatrix::LFO1, ModMatrix::Cutoff, 0.5f);
    REQUIRE (delayTail (a, 0.0f) == Catch::Approx (delayTail (b, 1.0f)).margin (1e-4));
}
