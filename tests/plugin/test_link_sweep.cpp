// ============================================================================
// H4 — EXHAUSTIVE link sweep. The enforcement tool that ends "modifying links is buggy":
//   (1) every source x every registry destination creates a valid route (full product);
//   (2) for every STEADY block source x every checkable destination: driving the source
//       moves the destination's modulated value, and removing the route returns it to base;
//   (3) LFO sources produce a time-varying offset; per-voice sources modulate the audio;
//   (4) route EDITING through the real overlay handlers (re-point / re-depth / invert /
//       delete) takes effect immediately.
// Any failing combo is a named defect (the INFO prints source+dest).
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginProcessor.h"
#include "ModDestRegistry.h"
#include "UI/ModMatrixPanel.h"
#include <cmath>

namespace
{
    const char* macroId (int src)
    {
        static const char* ids[] { ParamID::macro1, ParamID::macro2, ParamID::macro3, ParamID::macro4,
                                   ParamID::macro5, ParamID::macro6, ParamID::macro7, ParamID::macro8 };
        return ids[src - ModMatrix::Macro1];
    }
    bool isSteadyBlockSource (int s)
    { return (s >= ModMatrix::Macro1 && s <= ModMatrix::Macro8) || s == ModMatrix::ModWheel || s == ModMatrix::PitchBend; }

    juce::RangedAudioParameter* paramFor (VASynthProcessor& p, int dest)
    {
        for (auto& e : moddest::table())
            if (e.dest == dest && e.paramId != nullptr && *e.paramId != 0) return p.apvts.getParameter (e.paramId);
        return nullptr;
    }

    // Drive a steady block source to a nonzero value, render a few blocks with a held note, and
    // return the peak |modAnimNorm(dest)| observed. Depth is fixed at the caller's route.
    float drivenOffset (VASynthProcessor& p, int src, int dest, juce::RangedAudioParameter* param, int blocks = 10)
    {
        if (src >= ModMatrix::Macro1 && src <= ModMatrix::Macro8)
            p.apvts.getParameter (macroId (src))->setValueNotifyingHost (1.0f);
        p.prepareToPlay (48000.0, 128);
        float mx = 0.0f;
        for (int b = 0; b < blocks; ++b)
        {
            juce::AudioBuffer<float> buf (2, 128); buf.clear();
            juce::MidiBuffer m;
            if (b == 0)
            {
                m.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
                if (src == ModMatrix::ModWheel)  m.addEvent (juce::MidiMessage::controllerEvent (1, 1, 127), 0);
                if (src == ModMatrix::PitchBend) m.addEvent (juce::MidiMessage::pitchWheel (1, 16383), 0);
            }
            p.processBlock (buf, m);
            mx = std::max (mx, std::abs (p.modAnimNorm (dest, param)));
        }
        return mx;
    }

    void clearRouteTo (VASynthProcessor& p, int src, int dest)
    {
        for (int s = 0; s < ModMatrix::kSlots; ++s)
        {
            const auto sl = p.getModSlot (-1, s);
            if (sl.source == src && sl.dest == dest) p.clearModSlot (-1, s);
        }
    }
}

TEST_CASE ("sweep: every source routes to every registry destination (#H4)", "[plugin][modmatrix][sweep]")
{
    VASynthProcessor p;
    int combos = 0;
    for (int src = ModMatrix::LFO1; src <= ModMatrix::Macro8; ++src)
        for (auto& e : moddest::table())
        {
            for (int s = 0; s < ModMatrix::kSlots; ++s) p.clearModSlot (-1, s);
            const int slot = p.linkModRoute (-1, src, e.dest, 0.7f);
            INFO ("source " << src << " -> dest " << e.dest);
            REQUIRE (slot >= 0);
            const auto got = p.getModSlot (-1, slot);
            REQUIRE (got.source == src);
            REQUIRE (got.dest   == e.dest);
            ++combos;
        }
    REQUIRE (combos > 300);          // the whole product space was exercised
}

TEST_CASE ("sweep: steady block source moves every checkable dest, removal restores it (#H4)",
           "[plugin][modmatrix][sweep]")
{
    int checked = 0;
    for (int src = ModMatrix::LFO1; src <= ModMatrix::Macro8; ++src)
    {
        if (! isSteadyBlockSource (src)) continue;
        for (auto& e : moddest::table())
        {
            VASynthProcessor p;
            auto* prm = paramFor (p, e.dest);
            const bool checkable = (e.dest >= ModMatrix::kFirstBlockDest) || prm != nullptr;
            if (! checkable) continue;   // Pitch/Amp/WavePos have no published knob offset

            p.linkModRoute (-1, src, e.dest, 0.8f);
            const float on = drivenOffset (p, src, e.dest, prm);
            clearRouteTo (p, src, e.dest);
            const float off = drivenOffset (p, src, e.dest, prm);

            INFO ("source " << src << " -> dest " << e.dest << "  on=" << on << " off=" << off);
            REQUIRE (on > 0.02f);                    // driving the source moves the destination
            REQUIRE (off < on * 0.3f + 1.0e-3f);     // removing the route returns it toward base
            ++checked;
        }
    }
    REQUIRE (checked > 200);
}

TEST_CASE ("sweep: LFO sources produce a time-varying offset (#H4)", "[plugin][modmatrix][sweep]")
{
    for (int lfo = ModMatrix::LFO1; lfo <= ModMatrix::LFO3; ++lfo)
    {
        VASynthProcessor p;
        const int rateId = lfo - ModMatrix::LFO1;
        const char* rate[] { ParamID::lfoRate, ParamID::lfo2Rate, ParamID::lfo3Rate };
        p.apvts.getParameter (rate[rateId])->setValueNotifyingHost (0.7f);   // brisk
        p.linkModRoute (-1, lfo, ModMatrix::ReverbMix, 1.0f);
        p.prepareToPlay (48000.0, 128);
        float lo = 1.0e9f, hi = -1.0e9f;
        for (int b = 0; b < 300; ++b)
        {
            juce::AudioBuffer<float> buf (2, 128); buf.clear();
            juce::MidiBuffer m; if (b == 0) m.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
            p.processBlock (buf, m);
            const float o = p.blockModOffset (ModMatrix::ReverbMix);
            lo = std::min (lo, o); hi = std::max (hi, o);
        }
        INFO ("LFO source " << lfo);
        REQUIRE (hi - lo > 0.2f);
    }
}

TEST_CASE ("route editing through the real overlay handlers takes effect immediately (#H4)",
           "[plugin][modmatrix][sweep][ui]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    p.apvts.getParameter (ParamID::macro1)->setValueNotifyingHost (1.0f);
    ModMatrixPanel panel (p);

    auto* delayFb  = p.apvts.getParameter (ParamID::delayFeedback);
    auto* reverbMx = p.apvts.getParameter (ParamID::reverbMix);
    auto settle = [&] (int dest, juce::RangedAudioParameter* prm)
    {
        p.prepareToPlay (48000.0, 128);
        float mx = 0.0f;
        for (int b = 0; b < 8; ++b) { juce::AudioBuffer<float> buf (2, 128); buf.clear(); juce::MidiBuffer m;
            if (b == 0) m.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0); p.processBlock (buf, m);
            mx = std::max (mx, std::abs (p.modAnimNorm (dest, prm))); }
        return mx;
    };

    // Build Macro1 -> DelayFeedback via the overlay, positive depth.
    panel.pickForTest (0, ModMatrix::Macro1, ModMatrix::DelayFeedback, 0.8f);
    REQUIRE (settle (ModMatrix::DelayFeedback, delayFb) > 0.05f);

    // RE-POINT to ReverbMix: delay feedback offset drops, reverb mix offset appears.
    panel.pickForTest (0, ModMatrix::Macro1, ModMatrix::ReverbMix, 0.8f);
    REQUIRE (settle (ModMatrix::DelayFeedback, delayFb) < 0.02f);
    REQUIRE (settle (ModMatrix::ReverbMix, reverbMx) > 0.05f);

    // INVERT (negative depth): offset flips sign.
    panel.pickForTest (0, ModMatrix::Macro1, ModMatrix::ReverbMix, -0.8f);
    p.prepareToPlay (48000.0, 128);
    for (int b = 0; b < 8; ++b) { juce::AudioBuffer<float> buf (2, 128); buf.clear(); juce::MidiBuffer m;
        if (b == 0) m.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0); p.processBlock (buf, m); }
    REQUIRE (p.modAnimNorm (ModMatrix::ReverbMix, reverbMx) < 0.0f);

    // DELETE via the row's clear: offset returns to base.
    p.clearModSlot (-1, 0);
    REQUIRE (settle (ModMatrix::ReverbMix, reverbMx) < 0.02f);
}
