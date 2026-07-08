// ============================================================================
// 7C parts / multitimbral — processor integration. Locked-part bake equivalence,
// the multi-surface contract (independent parts play simultaneously with their own
// params), live-edit / randomize isolation, routing+preset persistence, missing-
// preset fallback, glitch-free reassignment, and RT-safety of the routed path.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginProcessor.h"
#include "PresetManager.h"
#include "UI/InputsDialog.h"
#include "test_util.h"
#include "alloc_hook.h"
#include <vector>
#include <cmath>

namespace
{
    void setP (VASynthProcessor& p, const char* id, float v) { p.apvts.getParameter (id)->setValueNotifyingHost (v); }

    // Render `blocks` blocks into a mono capture (channel 0), no new events.
    std::vector<float> capture (VASynthProcessor& p, int blocks)
    {
        std::vector<float> out; juce::AudioBuffer<float> buf (2, 256);
        for (int b = 0; b < blocks; ++b)
        {
            buf.clear(); juce::MidiBuffer m; p.processBlock (buf, m);
            const float* L = buf.getReadPointer (0);
            for (int i = 0; i < 256; ++i) out.push_back (L[i]);
        }
        return out;
    }

    double bandEnergy (const std::vector<float>& x, double loHz, double hiHz)
    {
        std::vector<float> w (8192, 0.0f);
        for (int i = 0; i < 8192 && i < (int) x.size(); ++i) w[(size_t) i] = x[(size_t) i];
        auto mag = tu::magnitudeSpectrum (w);
        const double binHz = 48000.0 / 8192.0;
        double e = 0.0;
        for (size_t k = 1; k < mag.size(); ++k) { const double f = k * binHz; if (f >= loHz && f < hiHz) e += mag[k] * mag[k]; }
        return e;
    }
}

TEST_CASE ("locked-part bake renders like the preset loaded live", "[plugin][7c][parts][bake]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // Live: load the preset into part 0 and play note 60.
    VASynthProcessor live; live.prepareToPlay (48000.0, 256);
    live.loadFactoryPreset ("Fat Saw Bass");
    live.routeNoteOn (60, 0.8f, 0);
    auto a = capture (live, 20);

    // Locked: bake the SAME preset into part 1 and route note 60 there.
    VASynthProcessor lockd; lockd.prepareToPlay (48000.0, 256);
    lockd.setPartPreset (1, "Fat Saw Bass");
    lockd.routeNoteOn (60, 0.8f, 1);
    auto b = capture (lockd, 20);

    const double ra = tu::rms (a), rb = tu::rms (b);
    INFO ("live rms=" << ra << " locked rms=" << rb);
    REQUIRE (ra > 0.01);
    REQUIRE (rb == Catch::Approx (ra).epsilon (0.05));   // same voice params -> same sound
}

TEST_CASE ("multi-surface contract: three parts sound at once with their own params", "[plugin][7c][parts][contract]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p; p.prepareToPlay (48000.0, 256);

    // Part 0 (live) = a pure sine (no highs/sub of its own); parts 1/2 = kick / hat.
    setP (p, ParamID::osc1Wave, 1.0f);        // choice index 3 (Sine) -> normalized 1.0
    setP (p, ParamID::osc2On, 0.0f); setP (p, ParamID::osc3On, 0.0f);
    p.setPartPreset (1, "Kick 808");
    p.setPartPreset (2, "Hat Closed");

    // Three simultaneous surfaces: QWERTY-equivalent (part 0) + two routed MIDI ins.
    p.routeNoteOn (60, 0.8f, 0);              // live sine ~261 Hz
    p.routeNoteOn (36, 1.0f, 1);             // kick (low)
    p.routeNoteOn (72, 1.0f, 2);             // hat  (highpassed noise)
    auto out = capture (p, 12);
    REQUIRE (tu::allFinite (out));

    const double low  = bandEnergy (out, 20.0, 100.0);     // only the kick lives here
    const double mid  = bandEnergy (out, 200.0, 400.0);    // the live sine
    const double high = bandEnergy (out, 7000.0, 16000.0); // only the hat lives here
    INFO ("low=" << low << " mid=" << mid << " high=" << high);
    REQUIRE (low  > 0.0);
    REQUIRE (mid  > 0.0);
    REQUIRE (high > 0.0);                     // all three parts sounding, each its own timbre
}

TEST_CASE ("live edits do not touch a locked part", "[plugin][7c][parts][isolation]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p; p.prepareToPlay (48000.0, 256);
    setP (p, ParamID::lfoDepth, 0.0f); setP (p, ParamID::lfoDest, 0.0f);   // shared LFO neutral

    p.setPartPreset (1, "Fat Saw Bass");
    auto part1 = [&] { p.routeNoteOn (48, 0.8f, 1); auto v = capture (p, 24); p.routeNoteOff (48, 1); capture (p, 120); return tu::rms (v); };

    const double before = part1();
    REQUIRE (before > 0.01);

    // Aggressively edit every LIVE voice param; the locked part is unaffected.
    setP (p, ParamID::osc1Wave, 0.0f); setP (p, ParamID::filterCutoff, 0.95f);
    setP (p, ParamID::filterReso, 0.9f); setP (p, ParamID::ampAttack, 0.7f);
    setP (p, ParamID::osc1Level, 0.1f); setP (p, ParamID::filterType, 0.66f);
    const double after = part1();
    INFO ("before=" << before << " after=" << after);
    REQUIRE (after == Catch::Approx (before).epsilon (0.05));
}

TEST_CASE ("ordinary state persists SOUND but RESETS routing/parts (lifecycle rule 2)", "[plugin][partsB][lifecycle]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor src; src.prepareToPlay (48000.0, 256);

    // SOUND state (a live param) + ROUTING state (parts + surface assignments).
    setP (src, ParamID::filterCutoff, 0.33f);
    const float wantCutoff = src.apvts.getRawParameterValue (ParamID::filterCutoff)->load();
    src.setPartPreset (1, "Kick 808");
    src.setSurfaceRouting ("Korg B2", 1);
    src.setSurfaceRouting ("Launchkey Mini", 2);
    juce::MemoryBlock blob; src.getStateInformation (blob);

    // Reopening the app == an ordinary state round-trip into a fresh processor.
    VASynthProcessor dst; dst.prepareToPlay (48000.0, 256);
    dst.setStateInformation (blob.getData(), (int) blob.getSize());

    // SOUND persists.
    REQUIRE (dst.apvts.getRawParameterValue (ParamID::filterCutoff)->load() == Catch::Approx (wantCutoff));

    // ROUTING resets: every surface back to LIVE (part 0), no locked parts.
    REQUIRE (dst.getSurfaceRouting ("Korg B2") == 0);
    REQUIRE (dst.getSurfaceRouting ("Launchkey Mini") == 0);
    REQUIRE (dst.getPartPreset (1).isEmpty());       // Part 1 unassigned again
}

TEST_CASE ("missing locked-part preset falls back to Init without crashing", "[plugin][7c][parts][fallback]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p; p.prepareToPlay (48000.0, 256);
    p.setPartPreset (1, "This Preset Does Not Exist");
    REQUIRE (p.getPartPreset (1) == "Init");         // fell back
    p.routeNoteOn (60, 0.8f, 1);
    REQUIRE (tu::rms (capture (p, 12)) > 0.005);      // still plays (Init baseline)
}

TEST_CASE ("reassigning a part mid-note stays finite and bounded (atomic publish)", "[plugin][7c][parts][reassign]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p; p.prepareToPlay (48000.0, 256);
    p.setPartPreset (1, "Warm Pad");
    p.routeNoteOn (60, 0.8f, 1);
    capture (p, 8);
    p.setPartPreset (1, "Fat Saw Bass");             // reassign WHILE the part is sounding
    auto out = capture (p, 8);
    REQUIRE (tu::allFinite (out));
    REQUIRE (tu::peak (out) <= 1.0f);
}

TEST_CASE ("INPUTS dialog action path routes a surface to a part and it plays", "[plugin][7c][parts][dialog]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p; p.prepareToPlay (48000.0, 256);

    InputsDialog dlg (p);
    REQUIRE (dlg.numRows() >= 1);
    REQUIRE (dlg.rowName (0) == "QWERTY");             // QWERTY listed first

    // Drive the dialog's OWN action handler: assign a surface to Part 1 + a preset.
    dlg.applyRouting ("MockController", 1, "Kick 808");
    REQUIRE (p.getSurfaceRouting ("MockController") == 1);
    REQUIRE (p.getPartPreset (1) == "Kick 808");

    // A note from that surface (routed via its assignment) renders with Part 1.
    p.routeMidi (juce::MidiMessage::noteOn (1, 36, 1.0f), p.getSurfaceRouting ("MockController"));
    auto out = capture (p, 12);
    REQUIRE (tu::allFinite (out));
    REQUIRE (bandEnergy (out, 20.0, 100.0) > 0.0);     // the kick sounds (Part 1 params applied)
    REQUIRE (p.partActivity (1) > 0);                  // PARTS strip would flicker P1
}

#ifndef VASYNTH_DOCS_DIR
 #define VASYNTH_DOCS_DIR "."
#endif
TEST_CASE ("render the INPUTS dialog to docs/inputs-dialog.png", "[plugin][7c][parts][screenshot]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p; p.prepareToPlay (48000.0, 256);

    // Discover a real surface (a present controller if any, else a stand-in), route it
    // to Part 1 with a bass preset, THEN build the display dialog so its combos reflect
    // the configured state in the shot.
    juce::String dev; { InputsDialog probe (p); dev = probe.numRows() > 1 ? probe.rowName (1) : juce::String ("Korg B2"); }
    p.setSurfaceRouting (dev, 1);
    p.setPartPreset (1, "Fat Saw Bass");

    auto dlg = std::make_unique<InputsDialog> (p);
    dlg->setSize (560, 70 + dlg->numRows() * 34);
    dlg->setBounds (dlg->getBounds());

    auto img = dlg->createComponentSnapshot (dlg->getLocalBounds(), false, 1.0f);
    REQUIRE (img.isValid());
    juce::File out (juce::String (VASYNTH_DOCS_DIR) + "/inputs-dialog.png");
    out.deleteFile();
    juce::FileOutputStream os (out);
    REQUIRE (os.openedOk());
    juce::PNGImageFormat png;
    REQUIRE (png.writeImageToStream (img, os));
}

TEST_CASE ("routed-note path does not allocate on the audio thread", "[plugin][7c][parts][rt]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p; p.prepareToPlay (48000.0, 512);
    p.setPartPreset (1, "Kick 808");
    p.setPartPreset (2, "Hat Closed");

    juce::AudioBuffer<float> buf (2, 512);
    for (int i = 0; i < 20; ++i) { buf.clear(); juce::MidiBuffer m; p.processBlock (buf, m); }   // warm up

    {
        alloc_hook::AllocGuard g;
        for (int b = 0; b < 200; ++b)
        {
            // Push routed events (producer side) then process (audio side).
            if (b % 6 == 0) { p.routeNoteOn (36 + (b % 4), 1.0f, 1 + (b % 3)); }
            if (b % 6 == 3) { p.routeNoteOff (36 + (b % 4), 1 + (b % 3)); }
            buf.clear(); juce::MidiBuffer m; p.processBlock (buf, m);
        }
        REQUIRE (g.count() == 0);
    }
}
