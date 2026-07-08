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

TEST_CASE ("routing + locked-part presets persist across a state round-trip", "[plugin][7c][parts][state]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor src; src.prepareToPlay (48000.0, 256);
    src.setPartPreset (1, "Kick 808");
    src.setPartPreset (2, "Warm Pad");
    src.setSurfaceRouting ("Korg B2", 1);
    src.setSurfaceRouting ("Launchkey Mini", 2);
    juce::MemoryBlock blob; src.getStateInformation (blob);

    VASynthProcessor dst; dst.prepareToPlay (48000.0, 256);
    dst.setStateInformation (blob.getData(), (int) blob.getSize());
    REQUIRE (dst.getPartPreset (1) == "Kick 808");
    REQUIRE (dst.getPartPreset (2) == "Warm Pad");
    REQUIRE (dst.getSurfaceRouting ("Korg B2") == 1);
    REQUIRE (dst.getSurfaceRouting ("Launchkey Mini") == 2);

    // The restored locked part actually plays (baked on load).
    dst.routeNoteOn (36, 1.0f, 1);
    REQUIRE (tu::rms (capture (dst, 10)) > 0.005);
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
