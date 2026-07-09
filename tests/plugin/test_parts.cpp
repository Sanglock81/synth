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
#include "AppInfo.h"
#include "UI/InputsDialog.h"
#include "UI/KitEditor.h"
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

TEST_CASE ("locked-part bake now includes the preset's FX (Sub-phase 2)", "[plugin][partsB2][bake]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // Live: Warm Pad (chorus + reverb + width) on part 0.
    VASynthProcessor live; live.prepareToPlay (48000.0, 256);
    live.loadFactoryPreset ("Warm Pad");
    live.routeNoteOn (60, 0.8f, 0); auto a = capture (live, 24);
    live.routeNoteOff (60, 0);      auto aTail = capture (live, 48);

    // Locked: bake Warm Pad into part 1 -> it must carry the same FX (a reverb tail).
    VASynthProcessor lockd; lockd.prepareToPlay (48000.0, 256);
    lockd.setPartPreset (1, "Warm Pad");
    lockd.routeNoteOn (60, 0.8f, 1); auto b = capture (lockd, 24);
    lockd.routeNoteOff (60, 1);      auto bTail = capture (lockd, 48);

    REQUIRE (tu::rms (a) == Catch::Approx (tu::rms (b)).epsilon (0.1));   // same voice + FX
    REQUIRE (tu::rms (aTail) > 0.002);                                   // live has a reverb tail...
    REQUIRE (tu::rms (bTail) > 0.002);                                   // ...and so does the baked locked part
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

namespace
{
    // Give part 0 a pure sine that decays fast and dry, so a routed/transposed note has
    // a measurable pitch AND a hung voice (missed note-off) is unambiguous vs silence.
    void makeLiveSine (VASynthProcessor& p)
    {
        setP (p, ParamID::osc1Wave, 1.0f);                 // Sine
        setP (p, ParamID::osc2On, 0.0f); setP (p, ParamID::osc3On, 0.0f);
        setP (p, ParamID::lfoDepth, 0.0f);
        setP (p, ParamID::ampRelease, 0.0f);               // near-instant release
        setP (p, ParamID::fxChorusOn, 0.0f); setP (p, ParamID::fxDelayOn, 0.0f);
        setP (p, ParamID::fxReverbOn, 0.0f); setP (p, ParamID::fxWidthOn, 0.0f);
    }
    double noteHz (int n) { return 440.0 * std::pow (2.0, (n - 69) / 12.0); }
}

TEST_CASE ("zones: unconfigured surface plays LIVE full-range", "[plugin][partsB][zones]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p; p.prepareToPlay (48000.0, 256);
    REQUIRE (p.getSurfaceZones ("QWERTY").empty());        // implicit default
    REQUIRE (p.getSurfaceRouting ("QWERTY") == 0);
    REQUIRE_FALSE (p.surfaceHasSplit ("QWERTY"));

    p.setPartPreset (1, "Kick 808");
    p.routeSurfaceMessage ("Anything", juce::MidiMessage::noteOn (1, 60, 0.8f));
    capture (p, 2);                                        // drain the routed FIFO on the audio thread
    REQUIRE (p.partActivity (0) > 0);                      // landed on LIVE, not the kick part
    REQUIRE (p.partActivity (1) == 0);
}

TEST_CASE ("zones: a key-range split routes each range to its part", "[plugin][partsB][zones][split]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p; p.prepareToPlay (48000.0, 256);
    makeLiveSine (p);
    p.setPartPreset (1, "Kick 808");

    // Bottom octave -> Part 1 (drums); the rest -> LIVE.
    p.setSurfaceZones ("Korg B2", { { 0, 47, 1, 0 }, { 48, 127, 0, 0 } });
    REQUIRE (p.surfaceHasSplit ("Korg B2"));

    p.routeSurfaceMessage ("Korg B2", juce::MidiMessage::noteOn (1, 36, 1.0f));   // low -> kick
    p.routeSurfaceMessage ("Korg B2", juce::MidiMessage::noteOn (1, 72, 0.8f));   // high -> live sine
    auto out = capture (p, 12);
    REQUIRE (tu::allFinite (out));
    REQUIRE (p.partActivity (1) > 0);                      // kick part hit
    REQUIRE (p.partActivity (0) > 0);                      // live part hit
    REQUIRE (bandEnergy (out, 20.0, 100.0) > 0.0);         // the kick's sub
}

TEST_CASE ("zones: transpose shifts the sounding note (trigger unchanged)", "[plugin][partsB][zones][transpose]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p; p.prepareToPlay (48000.0, 256);
    makeLiveSine (p);
    p.setSurfaceZones ("K", { { 0, 127, 0, +12 } });       // whole surface up an octave

    p.routeSurfaceMessage ("K", juce::MidiMessage::noteOn (1, 48, 0.9f));   // trigger 48 -> sounds 60
    auto out = capture (p, 16);
    const double at60 = bandEnergy (out, noteHz (60) - 12, noteHz (60) + 12);
    const double at48 = bandEnergy (out, noteHz (48) - 8,  noteHz (48) + 8);
    INFO ("energy@60=" << at60 << " energy@48=" << at48);
    REQUIRE (at60 > at48 * 8.0);                           // sounds an octave up
}

TEST_CASE ("zones: transpose clamps at the MIDI extremes", "[plugin][partsB][zones][transpose]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p; p.prepareToPlay (48000.0, 256);
    makeLiveSine (p);
    p.setSurfaceZones ("K", { { 0, 127, 0, +48 } });
    p.routeSurfaceMessage ("K", juce::MidiMessage::noteOn (1, 120, 0.9f));  // 120+48 -> clamp 127
    p.routeSurfaceMessage ("K", juce::MidiMessage::noteOff (1, 120));       // must release the clamped note
    auto tail = capture (p, 200);
    REQUIRE (tu::rms (tail) < 1e-3);                       // no hung voice -> note-off matched the clamp
}

TEST_CASE ("zones: note-off releases the note-on's zone even after a re-split (ledger)", "[plugin][partsB][zones][ledger]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p; p.prepareToPlay (48000.0, 256);
    makeLiveSine (p);

    p.setSurfaceZones ("K", { { 0, 127, 0, +7 } });        // note-on transposes +7
    p.routeSurfaceMessage ("K", juce::MidiMessage::noteOn (1, 50, 0.9f));   // sounds 57
    REQUIRE (tu::rms (capture (p, 8)) > 0.001);

    p.setSurfaceZones ("K", { { 0, 127, 0, 0 } });         // zones change WHILE held
    p.routeSurfaceMessage ("K", juce::MidiMessage::noteOff (1, 50));        // must release 57, not 50
    auto tail = capture (p, 220);
    REQUIRE (tu::rms (tail) < 1e-3);                       // released cleanly (ledger snapshot honoured)
}

TEST_CASE ("zones: setSurfaceZones normalises to a contiguous tiling of [0,127]", "[plugin][partsB][zones][normalise]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p; p.prepareToPlay (48000.0, 256);

    // Gappy + out-of-order + overlapping input.
    p.setSurfaceZones ("K", { { 60, 80, 2, 0 }, { 0, 40, 1, 0 } });
    auto z = p.getSurfaceZones ("K");
    REQUIRE (z.size() >= 2);
    REQUIRE (z.front().loNote == 0);                       // covers the bottom
    REQUIRE (z.back().hiNote == 127);                      // covers the top
    for (std::size_t i = 1; i < z.size(); ++i)
        REQUIRE (z[i].loNote == z[i - 1].hiNote + 1);      // contiguous, no gaps/overlap
}

TEST_CASE ("zones: add/remove split adjust the tiling and stay gapless", "[plugin][partsB][zones][split]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p; p.prepareToPlay (48000.0, 256);

    p.addSurfaceSplit ("K", 60);                          // one seam -> two zones
    auto z = p.getSurfaceZones ("K");
    REQUIRE (z.size() == 2);
    REQUIRE (z[0].loNote == 0);  REQUIRE (z[0].hiNote == 59);
    REQUIRE (z[1].loNote == 60); REQUIRE (z[1].hiNote == 127);

    p.addSurfaceSplit ("K", 96);                          // split the upper zone again
    REQUIRE (p.getSurfaceZones ("K").size() == 3);

    p.removeSurfaceSplit ("K", 1);                        // merge the middle away
    z = p.getSurfaceZones ("K");
    REQUIRE (z.size() == 2);
    REQUIRE (z.front().loNote == 0);
    REQUIRE (z.back().hiNote == 127);
    for (std::size_t i = 1; i < z.size(); ++i) REQUIRE (z[i].loNote == z[i - 1].hiNote + 1);
}

TEST_CASE ("zones: split-by-play exposes the last note per surface", "[plugin][partsB][zones][byplay]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p; p.prepareToPlay (48000.0, 256);
    REQUIRE (p.lastNoteForSurface ("K") == -1);
    p.routeSurfaceMessage ("K", juce::MidiMessage::noteOn (1, 53, 0.7f));
    REQUIRE (p.lastNoteForSurface ("K") == 53);           // the dialog arms, reads this, seams here
    p.addSurfaceSplit ("K", p.lastNoteForSurface ("K"));
    REQUIRE (p.surfaceHasSplit ("K"));
}

TEST_CASE ("zones: reset returns a surface (and all routing) to default", "[plugin][partsB][zones][reset]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p; p.prepareToPlay (48000.0, 256);
    p.setSurfaceZones ("K", { { 0, 59, 1, 0 }, { 60, 127, 0, 0 } });
    p.setSurfaceRouting ("J", 2);

    p.resetSurfaceZones ("K");
    REQUIRE_FALSE (p.surfaceHasSplit ("K"));
    REQUIRE (p.getSurfaceRouting ("K") == 0);

    p.resetAllRouting();
    REQUIRE (p.getSurfaceZones ("J").empty());
    REQUIRE (p.getSurfaceRouting ("J") == 0);
}

TEST_CASE ("zones: QWERTY is a splittable surface like any other (rule 5)", "[plugin][partsB][zones][qwerty]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p; p.prepareToPlay (48000.0, 256);
    makeLiveSine (p);
    p.setPartPreset (1, "Kick 808");

    // Bottom octave of the computer keyboard -> Part 1 (drums); rest -> LIVE.
    p.setSurfaceZones ("QWERTY", { { 0, 47, 1, 0 }, { 48, 127, 0, 0 } });

    p.routeSurfaceMessage ("QWERTY", juce::MidiMessage::noteOn (1, 36, 1.0f));   // low -> kick
    p.routeSurfaceMessage ("QWERTY", juce::MidiMessage::noteOn (1, 60, 0.8f));   // mid -> live
    capture (p, 8);
    REQUIRE (p.partActivity (1) > 0);
    REQUIRE (p.partActivity (0) > 0);
}

TEST_CASE ("kit part: a routed trigger fires its pad through the dispatch path", "[plugin][kitpart]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p; p.prepareToPlay (48000.0, 256);

    VASynthProcessor::KitDefinition kit; kit.name = "Test Kit";
    kit.pads[0] = { 36, "Kick 808", { 36, 0, 0, 0 }, 1, 1.0f, 0 };
    kit.pads[1] = { 42, "Hat Closed", { 42, 0, 0, 0 }, 1, 1.0f, 1 };
    p.setPartKit (1, kit);
    REQUIRE (p.isPartKit (1));
    REQUIRE (p.getPartPreset (1) == "Test Kit");          // PARTS strip shows the kit name

    p.routeMidi (juce::MidiMessage::noteOn (1, 36, 1.0f), 1);   // trigger the kick pad
    auto out = capture (p, 12);
    REQUIRE (tu::allFinite (out));
    REQUIRE (p.partActivity (1) > 0);
    REQUIRE (bandEnergy (out, 20.0, 110.0) > 0.0);        // the kick sub

    // Assigning a plain preset turns the kit off.
    p.setPartPreset (1, "Warm Pad");
    REQUIRE_FALSE (p.isPartKit (1));
    REQUIRE (p.getPartKit (1).pads[0].triggerNote == -1); // definition forgotten
}

TEST_CASE ("kit part: a pad with a missing source bakes Init and still plays", "[plugin][kitpart][fallback]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p; p.prepareToPlay (48000.0, 256);
    VASynthProcessor::KitDefinition kit; kit.name = "K";
    kit.pads[0] = { 36, "No Such Preset", { 60, 0, 0, 0 }, 1, 1.0f, 0 };
    p.setPartKit (1, kit);
    p.routeMidi (juce::MidiMessage::noteOn (1, 36, 1.0f), 1);
    REQUIRE (tu::rms (capture (p, 12)) > 0.001);          // Init baseline sounds, no crash
}

TEST_CASE ("kit: trigger path is allocation-free; publish mid-note is glitch-free", "[plugin][kitpart][rt]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p; p.prepareToPlay (48000.0, 512);
    p.setPartKit (1, p.loadKit ("808 Basics"));

    juce::AudioBuffer<float> buf (2, 512);
    for (int i = 0; i < 20; ++i) { buf.clear(); juce::MidiBuffer m; p.processBlock (buf, m); }   // warm up

    {
        alloc_hook::AllocGuard g;
        for (int b = 0; b < 200; ++b)
        {
            if (b % 4 == 0) p.routeMidi (juce::MidiMessage::noteOn  (1, 36 + (b % 6), 1.0f), 1);
            if (b % 4 == 2) p.routeMidi (juce::MidiMessage::noteOff (1, 36 + (b % 6)),       1);
            buf.clear(); juce::MidiBuffer m; p.processBlock (buf, m);
        }
        REQUIRE (g.count() == 0);
    }

    // Re-publish a different kit WHILE pads are sounding -> finite and bounded.
    p.routeMidi (juce::MidiMessage::noteOn (1, 36, 1.0f), 1);
    capture (p, 4);
    p.setPartKit (1, p.loadKit ("Stab Board"));
    auto out = capture (p, 8);
    REQUIRE (tu::allFinite (out));
    REQUIRE (tu::peak (out) <= 1.0f);
}

TEST_CASE ("kit: factory kits load and play their pads", "[plugin][kitpart][factory]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p; p.prepareToPlay (48000.0, 256);
    REQUIRE (p.getKitNames().contains ("808 Basics"));
    REQUIRE (p.getKitNames().contains ("Stab Board"));

    p.setPartKit (1, p.loadKit ("808 Basics"));
    REQUIRE (p.isPartKit (1));
    p.routeMidi (juce::MidiMessage::noteOn (1, 36, 1.0f), 1);   // kick trigger
    REQUIRE (bandEnergy (capture (p, 12), 20.0, 110.0) > 0.0);

    // Stab Board: a chord-pad trigger fires a 3-note stab.
    p.setPartKit (2, p.loadKit ("Stab Board"));
    p.routeMidi (juce::MidiMessage::noteOn (1, 40, 1.0f), 2);   // first stab pad
    capture (p, 6);
    REQUIRE (p.partActivity (2) > 0);
}

TEST_CASE ("kit: kit-preset save/load round-trips", "[plugin][kitpart][preset]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const juce::String nm = "__test_kit__";
    auto file = AppInfo::kitDir().getChildFile (nm + ".kit");
    file.deleteFile();

    VASynthProcessor p; p.prepareToPlay (48000.0, 256);
    VASynthProcessor::KitDefinition def; def.name = nm;
    def.pads[0] = { 36, "Kick 808", { 36, 0, 0, 0 }, 1, 0.8f, 0 };
    def.pads[1] = { 60, "Synth Pluck", { 60, 63, 67, 0 }, 3, 1.0f, 0 };
    REQUIRE (p.saveKit (nm, def));
    REQUIRE (p.getKitNames().contains (nm));

    auto back = p.loadKit (nm);
    REQUIRE (back.pads[0].source == "Kick 808");
    REQUIRE (back.pads[0].level == Catch::Approx (0.8f));
    REQUIRE (back.pads[1].numSound == 3);
    REQUIRE (back.pads[1].soundNote[1] == 63);
    file.deleteFile();
}

TEST_CASE ("kit: a kit part round-trips through MULTI", "[plugin][kitpart][multi]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor src; src.prepareToPlay (48000.0, 256);
    src.setPartKit (1, src.loadKit ("808 Basics"));
    src.setSurfaceZones ("B2", { { 0, 47, 1, 0 }, { 48, 127, 0, 0 } });   // bottom octave -> the kit
    auto multi = src.captureMultiState();

    VASynthProcessor dst; dst.prepareToPlay (48000.0, 256);
    dst.applyMultiState (multi);
    REQUIRE (dst.isPartKit (1));
    REQUIRE (dst.getPartKit (1).name == "808 Basics");
    REQUIRE (dst.getPartKit (1).pads[3].chokeGroup == 1);         // hats choke survived
    REQUIRE (dst.surfaceHasSplit ("B2"));
}

TEST_CASE ("MULTI captures + reapplies the layout (parts + surface zones)", "[plugin][partsB][multi]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor src; src.prepareToPlay (48000.0, 256);
    src.setPartPreset (1, "Kick 808");
    src.setSurfaceZones ("Korg B2", { { 0, 47, 1, 0 }, { 48, 127, 0, +12 } });
    src.setSurfaceRouting ("Launchkey", 0);
    auto multi = src.captureMultiState();

    // A fresh instance recalls it ONLY via applyMultiState.
    VASynthProcessor dst; dst.prepareToPlay (48000.0, 256);
    REQUIRE_FALSE (dst.surfaceHasSplit ("Korg B2"));         // nothing yet
    dst.applyMultiState (multi);

    REQUIRE (dst.getPartPreset (1) == "Kick 808");
    REQUIRE (dst.surfaceHasSplit ("Korg B2"));
    auto z = dst.getSurfaceZones ("Korg B2");
    REQUIRE (z.size() == 2);
    REQUIRE (z[0].part == 1);
    REQUIRE (z[1].transpose == 12);
}

TEST_CASE ("MULTI apply repoints a zone off a missing preset to LIVE", "[plugin][partsB][multi][fallback]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p; p.prepareToPlay (48000.0, 256);

    juce::ValueTree multi ("MULTI");
    juce::ValueTree part ("PART"); part.setProperty ("index", 1, nullptr);
    part.setProperty ("preset", "No Such Preset", nullptr); multi.addChild (part, -1, nullptr);
    juce::ValueTree surf ("SURFACE"); surf.setProperty ("name", "K", nullptr);
    juce::ValueTree zone ("ZONE"); zone.setProperty ("lo", 0, nullptr); zone.setProperty ("hi", 127, nullptr);
    zone.setProperty ("part", 1, nullptr); zone.setProperty ("transpose", 0, nullptr);
    surf.addChild (zone, -1, nullptr); multi.addChild (surf, -1, nullptr);

    p.applyMultiState (multi);
    REQUIRE (p.getSurfaceRouting ("K") == 0);               // zone fell back to LIVE
}

TEST_CASE ("MULTI apply clears parts/surfaces not named in the layout", "[plugin][partsB][multi]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p; p.prepareToPlay (48000.0, 256);
    p.setPartPreset (2, "Warm Pad");
    p.setSurfaceRouting ("Old", 2);

    p.applyMultiState (juce::ValueTree ("MULTI"));           // empty layout
    REQUIRE (p.getPartPreset (2).isEmpty());
    REQUIRE (p.getSurfaceRouting ("Old") == 0);
}

TEST_CASE ("MULTI save/load round-trips through a file", "[plugin][partsB][multi][file]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const juce::String nm = "__test_multi__";
    auto file = AppInfo::multiDir().getChildFile (nm + ".multi");
    file.deleteFile();

    VASynthProcessor src; src.prepareToPlay (48000.0, 256);
    src.setPartPreset (1, "Fat Saw Bass");
    src.setSurfaceZones ("B2", { { 0, 59, 1, 0 }, { 60, 127, 0, 0 } });
    REQUIRE (src.saveMulti (nm));
    REQUIRE (src.getMultiNames().contains (nm));

    VASynthProcessor dst; dst.prepareToPlay (48000.0, 256);
    REQUIRE (dst.loadMulti (nm));
    REQUIRE (dst.getPartPreset (1) == "Fat Saw Bass");
    REQUIRE (dst.surfaceHasSplit ("B2"));

    file.deleteFile();                                       // clean up the test artefact
}

namespace
{
    bool anyFocusable (juce::Component& c)
    {
        if (c.getWantsKeyboardFocus()) return true;
        for (auto* ch : c.getChildren()) if (anyFocusable (*ch)) return true;
        return false;
    }
}

TEST_CASE ("kit editor refuses keyboard focus (QWERTY plays for learn-by-play)", "[plugin][kitpart][focus]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p; p.prepareToPlay (48000.0, 256);
    p.setPartKit (1, p.loadKit ("808 Basics"));
    KitEditor ed (p, 1);
    ed.setSize (660, 560);
    REQUIRE_FALSE (anyFocusable (ed));                    // nothing steals focus while open
}

#ifndef VASYNTH_DOCS_DIR
 #define VASYNTH_DOCS_DIR "."
#endif
TEST_CASE ("render the KIT EDITOR to docs/kit-editor.png", "[plugin][kitpart][screenshot]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p; p.prepareToPlay (48000.0, 256);
    p.setPartKit (1, p.loadKit ("808 Basics"));

    auto ed = std::make_unique<KitEditor> (p, 1);
    ed->setSize (660, 560);
    ed->setBounds (ed->getBounds());

    auto img = ed->createComponentSnapshot (ed->getLocalBounds(), false, 1.0f);
    REQUIRE (img.isValid());
    juce::File out (juce::String (VASYNTH_DOCS_DIR) + "/kit-editor.png");
    out.deleteFile();
    juce::FileOutputStream os (out);
    REQUIRE (os.openedOk());
    juce::PNGImageFormat png;
    REQUIRE (png.writeImageToStream (img, os));
}

TEST_CASE ("render the INPUTS dialog to docs/inputs-dialog.png", "[plugin][7c][parts][screenshot]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p; p.prepareToPlay (48000.0, 256);

    // Configure a headline split: QWERTY bottom octave -> Part 1 (bass), rest -> LIVE.
    p.setPartPreset (1, "Fat Saw Bass");
    p.setSurfaceZones ("QWERTY", { { 0, 47, 1, +0 }, { 48, 127, 0, 0 } });

    auto dlg = std::make_unique<InputsDialog> (p);
    dlg->expandSurface ("QWERTY");                          // show the zone editor open
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
