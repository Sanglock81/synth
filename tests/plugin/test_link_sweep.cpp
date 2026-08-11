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

// #12: PER-VOICE sources (env / velocity / note / random) must also animate their target. The
// processor publishes a representative live-voice snapshot into the ANIMATION mod-source set, so a
// route from these sources is no longer silent in the UI (previously only block sources animated).
TEST_CASE ("sweep: per-voice sources (env/vel/note/random) animate their dest (#12)", "[plugin][modmatrix][sweep]")
{
    struct Src { int id; const char* name; };
    const Src voiceSrcs[] { { ModMatrix::ModEnv, "ModEnv" }, { ModMatrix::AmpEnv, "AmpEnv" },
                            { ModMatrix::Velocity, "Velocity" }, { ModMatrix::Note, "Note" },
                            { ModMatrix::Random, "Random" } };
    const int dests[] { ModMatrix::Cutoff, ModMatrix::ReverbMix };   // a voice-tier AND a block-tier dest
    int checked = 0;
    for (auto sc : voiceSrcs)
        for (int dest : dests)
        {
            VASynthProcessor p;
            auto* prm = paramFor (p, dest);
            p.linkModRoute (-1, sc.id, dest, 0.9f);
            p.apvts.getParameter (ParamID::ampSustain)->setValueNotifyingHost (1.0f);
            p.prepareToPlay (48000.0, 128);
            float mx = 0.0f;
            const int notes[] { 72, 64, 79, 67 };    // non-zero noteNorm; several notes so Random S&H varies
            for (int ni = 0; ni < 4; ++ni)
            {
                { juce::AudioBuffer<float> b (2, 128); b.clear(); juce::MidiBuffer on;
                  on.addEvent (juce::MidiMessage::noteOn (1, notes[ni], 0.92f), 0); p.processBlock (b, on); }
                for (int bl = 0; bl < 8; ++bl)
                { juce::AudioBuffer<float> b (2, 128); b.clear(); juce::MidiBuffer m; p.processBlock (b, m);
                  mx = std::max (mx, std::abs (p.modAnimNorm (dest, prm))); }
                { juce::AudioBuffer<float> b (2, 128); b.clear(); juce::MidiBuffer off;
                  off.addEvent (juce::MidiMessage::noteOff (1, notes[ni]), 0); p.processBlock (b, off); }
            }
            INFO (sc.name << " -> dest " << dest << "  peak |offset| = " << mx);
            REQUIRE (mx > 0.02f);                    // the per-voice source moves (animates) the destination
            ++checked;
        }
    REQUIRE (checked == 10);
}

TEST_CASE ("sweep: LFO sources produce a time-varying offset (#H4)", "[plugin][modmatrix][sweep]")
{
    for (int lfo = ModMatrix::LFO1; lfo <= ModMatrix::LFO3; ++lfo)
    {
        VASynthProcessor p;
        const int rateId = lfo - ModMatrix::LFO1;
        const char* rate[] { ParamID::lfoRate, ParamID::lfo2Rate, ParamID::lfo3Rate };
        const char* dest[] { ParamID::lfoDest, ParamID::lfo2Dest, ParamID::lfo3Dest };
        p.apvts.getParameter (rate[rateId])->setValueNotifyingHost (0.7f);   // brisk
        p.apvts.getParameter (dest[rateId])->setValueNotifyingHost (1.0f);   // dest = On: enable as a LINK source
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

// LINK P0 — the regression the old #H4 LFO test masked by pre-setting DEST=On. An LFO must publish
// its matrix source and MODULATE regardless of its fixed DEST (Off/Pitch/Cutoff/On) and SYNC state,
// from any route-creation path — the previously-silent cells were routes made with DEST left at the
// default Off (MOD overlay add, a loaded preset, RANDOM). Every cell here uses linkModRoute (the
// non-auto-enable path) and a plain completeModLink gesture; each must move the destination.
TEST_CASE ("sweep: an LFO route modulates from ANY fixed-DEST + SYNC state, incl. DEST=Off (LINK P0)",
           "[plugin][modmatrix][sweep][lfo]")
{
    struct D { int dest; const char* name; };
    const D dests[] { { ModMatrix::Cutoff, "Cutoff" }, { ModMatrix::ReverbMix, "ReverbMix" },
                      { ModMatrix::EqB3Gain, "EqB3Gain" }, { ModMatrix::StereoWidth, "StereoWidth" },
                      { ModMatrix::DelayFeedback, "DelayFeedback" }, { ModMatrix::ChorusDepth, "ChorusDepth" } };
    const char* stateName[] { "Off", "Pitch", "Cutoff", "On" };
    const char* rateIds[] { ParamID::lfoRate, ParamID::lfo2Rate, ParamID::lfo3Rate };
    const char* destIds[] { ParamID::lfoDest, ParamID::lfo2Dest, ParamID::lfo3Dest };
    const char* syncIds[] { ParamID::lfoSync, ParamID::lfo2Sync, ParamID::lfo3Sync };
    const char* divIds[]  { ParamID::lfoDiv,  ParamID::lfo2Div,  ParamID::lfo3Div };

    // Peak-to-peak of the destination's modulated offset (modAnimNorm works for BOTH voice-tier
    // dests like Cutoff and block-tier dests like ReverbMix — the metric the steady sweep validated).
    auto rangeOf = [] (VASynthProcessor& p, int dest, juce::RangedAudioParameter* param)
    {
        p.prepareToPlay (48000.0, 128);
        float lo = 1.0e9f, hi = -1.0e9f;
        for (int b = 0; b < 220; ++b)
        {
            juce::AudioBuffer<float> buf (2, 128); buf.clear();
            juce::MidiBuffer m; if (b == 0) m.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
            p.processBlock (buf, m);
            const float o = p.modAnimNorm (dest, param); lo = std::min (lo, o); hi = std::max (hi, o);
        }
        return hi - lo;
    };

    int cells = 0;
    for (int li = 0; li < 3; ++li)
        for (int ds = 0; ds <= 3; ++ds)                  // the LFO's OWN fixed DEST: Off / Pitch / Cutoff / On
            for (int sync = 0; sync <= 1; ++sync)
                for (auto& d : dests)
                {
                    VASynthProcessor p;
                    auto setChoice = [&] (const char* id, float idx)
                    { auto* pr = p.apvts.getParameter (id); pr->setValueNotifyingHost (pr->convertTo0to1 (idx)); };
                    p.apvts.getParameter (rateIds[li])->setValueNotifyingHost (0.7f);   // brisk free rate
                    setChoice (destIds[li], (float) ds);                                // fixed DEST state under test
                    p.apvts.getParameter (syncIds[li])->setValueNotifyingHost (sync ? 1.0f : 0.0f);
                    setChoice (divIds[li], 5.0f);                                       // ~1/8 for the synced case
                    p.linkModRoute (-1, ModMatrix::LFO1 + li, d.dest, 1.0f);            // the non-auto-enable path
                    const float range = rangeOf (p, d.dest, paramFor (p, d.dest));
                    INFO ("LFO" << (li + 1) << "  entry=linkModRoute  DEST=" << stateName[ds]
                          << "  SYNC=" << (sync ? "on" : "off") << "  -> " << d.name << "  range=" << range);
                    REQUIRE (range > 0.02f);                                            // modulates in EVERY cell
                    ++cells;
                }
    REQUIRE (cells == 3 * 4 * 2 * 6);

    // The LINK-button gesture path (arm the source, tap the knob) with DEST left at its default Off.
    {
        VASynthProcessor p;
        p.apvts.getParameter (ParamID::lfoRate)->setValueNotifyingHost (0.7f);
        p.armModLink (ModMatrix::LFO1);
        REQUIRE (p.completeModLink (ModMatrix::ReverbMix) >= 0);   // arm->tap makes a route
        INFO ("completeModLink gesture, DEST default Off");
        REQUIRE (rangeOf (p, ModMatrix::ReverbMix, paramFor (p, ModMatrix::ReverbMix)) > 0.02f);   // ...that actually modulates
    }
}

// LINK P0 mixer-tier dests: PartLevel (tremolo) + PartPan (equal-power auto-pan), layered on the
// untouched linear base pan law. Focus-scoped for now (a per-part rework lands before route presets).
namespace
{
    double blkRms (const juce::AudioBuffer<float>& b, int ch)
    {
        double acc = 0.0; for (int i = 0; i < b.getNumSamples(); ++i) { const float s = b.getSample (ch, i); acc += (double) s * s; }
        return std::sqrt (acc / b.getNumSamples());
    }
}

TEST_CASE ("PartLevel dest tremolos the output (floored, never silent); PartPan auto-pans it (LINK P0)",
           "[plugin][modmatrix][mixer]")
{
    {   // PartLevel -> tremolo: the output amplitude PULSES, floored (-12 dB) so it never gates out.
        VASynthProcessor p;
        p.apvts.getParameter (ParamID::lfoRate)->setValueNotifyingHost (0.6f);
        p.apvts.getParameter (ParamID::ampSustain)->setValueNotifyingHost (1.0f);
        p.linkModRoute (-1, ModMatrix::LFO1, ModMatrix::PartLevel, 1.0f);
        p.prepareToPlay (48000.0, 128);
        float lo = 1.0e9f, hi = -1.0e9f;
        for (int b = 0; b < 280; ++b)
        {
            juce::AudioBuffer<float> buf (2, 128); buf.clear(); juce::MidiBuffer m;
            if (b == 0) m.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
            p.processBlock (buf, m);
            if (b > 30) { const float r = (float) blkRms (buf, 0); lo = std::min (lo, r); hi = std::max (hi, r); }
        }
        INFO ("PartLevel tremolo RMS lo=" << lo << " hi=" << hi);
        REQUIRE (hi > 1.0e-3f);            // there is sound
        REQUIRE (hi - lo > 0.10f * hi);    // amplitude pulses by a clear margin (tremolo)
        REQUIRE (lo > 0.0f);               // floored: never fully gates to silence
    }
    {   // PartPan -> auto-pan: the L/R balance swings and crosses centre (pans both ways).
        VASynthProcessor p;
        p.apvts.getParameter (ParamID::lfoRate)->setValueNotifyingHost (0.6f);
        p.apvts.getParameter (ParamID::ampSustain)->setValueNotifyingHost (1.0f);
        p.linkModRoute (-1, ModMatrix::LFO1, ModMatrix::PartPan, 1.0f);
        p.prepareToPlay (48000.0, 128);
        float lo = 1.0e9f, hi = -1.0e9f;
        for (int b = 0; b < 280; ++b)
        {
            juce::AudioBuffer<float> buf (2, 128); buf.clear(); juce::MidiBuffer m;
            if (b == 0) m.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
            p.processBlock (buf, m);
            if (b > 30) { const float bal = (float) (blkRms (buf, 0) - blkRms (buf, 1)); lo = std::min (lo, bal); hi = std::max (hi, bal); }
        }
        INFO ("PartPan balance (L-R RMS) lo=" << lo << " hi=" << hi);
        REQUIRE (hi - lo > 0.05f);          // the balance swings (auto-pan)
        REQUIRE (hi > 0.0f); REQUIRE (lo < 0.0f);   // crosses centre: pans both ways
    }
}

// TARGET STATE (rider c): a BACKGROUND part's own PartLevel route must tremolo it while edit focus is
// elsewhere. Block-tier mods are FOCUS-SCOPED today, so this FAILS now — [!shouldfail] keeps the gate
// green and AUTO-FLAGS the moment the per-part block-mod rework lands (remove the tag then).
TEST_CASE ("multi-part: a background part's PartLevel route tremolos it with focus elsewhere (target state)",
           "[plugin][modmatrix][mixer][!shouldfail]")
{
    VASynthProcessor p;                                        // edit focus defaults to part 0
    p.apvts.getParameter (ParamID::lfoRate)->setValueNotifyingHost (0.6f);
    p.apvts.getParameter (ParamID::ampSustain)->setValueNotifyingHost (1.0f);
    p.linkModRoute (1, ModMatrix::LFO1, ModMatrix::PartLevel, 1.0f);   // route on part 1 (NOT the edit focus, which is 0)
    p.prepareToPlay (48000.0, 128);
    p.routeNoteOn (60, 0.9f, 1);                               // sound part 1 (background) ONCE, then hold it
    // CHUNK-average the RMS: a tonal note's per-block RMS wobbles from partial-cycle windowing;
    // averaging over 20 blocks cancels that, so only a REAL tremolo makes the chunk means diverge.
    double acc = 0.0; int n = 0; float lo = 1.0e9f, hi = -1.0e9f, level = 0.0f;
    for (int b = 0; b < 340; ++b)
    {
        juce::AudioBuffer<float> buf (2, 128); buf.clear(); juce::MidiBuffer m;
        p.processBlock (buf, m);
        if (b >= 60) { acc += blkRms (buf, 0); level = std::max (level, (float) blkRms (buf, 0));
                       if (++n == 20) { const float cm = (float) (acc / n); lo = std::min (lo, cm); hi = std::max (hi, cm); acc = 0.0; n = 0; } }
    }
    INFO ("background-part chunk-RMS lo=" << lo << " hi=" << hi << " (level " << level << ")");
    REQUIRE (level > 1.0e-3f);           // part 1 sounds
    REQUIRE (hi - lo > 0.08f * hi);      // ...and its PartLevel route tremolos it (FAILS until per-part rework)
}

// Rider 2 headroom: a full-depth auto-pan rides one channel to +3 dB. On a normal patch the master
// safety clipper must not be AUDIBLY engaged (peak stays under the ceiling; near-zero samples clipped).
TEST_CASE ("PartPan auto-pan headroom: full depth does not audibly engage the safety clipper (rider 2)",
           "[plugin][modmatrix][mixer][headroom]")
{
    VASynthProcessor p;
    p.apvts.getParameter (ParamID::ampSustain)->setValueNotifyingHost (1.0f);
    p.apvts.getParameter (ParamID::lfoRate)->setValueNotifyingHost (0.5f);
    p.linkModRoute (-1, ModMatrix::LFO1, ModMatrix::PartPan, 1.0f);
    p.prepareToPlay (48000.0, 128);
    float peak = 0.0f; long clipped = 0, total = 0;
    for (int b = 0; b < 320; ++b)
    {
        juce::AudioBuffer<float> buf (2, 128); buf.clear(); juce::MidiBuffer m;
        if (b == 0) { m.addEvent (juce::MidiMessage::noteOn (1, 48, 1.0f), 0);   // a chord = hot input
                      m.addEvent (juce::MidiMessage::noteOn (1, 55, 1.0f), 0);
                      m.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0); }
        p.processBlock (buf, m);
        if (b > 30) for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < 128; ++i)
        { const float a = std::abs (buf.getSample (ch, i)); peak = std::max (peak, a); ++total; if (a >= 0.999f) ++clipped; }
    }
    INFO ("auto-pan peak=" << peak << "  clipped=" << clipped << "/" << total);
    REQUIRE (peak <= 1.0f);                              // the ceiling holds (safety clipper)
    REQUIRE ((double) clipped / (double) total < 0.01); // ...but is not audibly engaged (<1% at ceiling)
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
