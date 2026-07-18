// ============================================================================
// I1 — the drum PADS of a controller (a profile-declared MIDI channel + note range)
// split off into their OWN routable input surface ("<device> Pads"), so the pads can
// play a different part than the keys. Driven by the device profile (Launchkey Mini:
// ch10, notes 36-51); routed through the same string-keyed surface machinery as any
// other surface, so it gets its own zones / activity / MULTI persistence for free.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "PluginProcessor.h"
#include "MidiProfile.h"

namespace
{
    constexpr double kSR = 48000.0;
    const juce::String kLK = "Novation Launchkey Mini";
    const juce::String kLKPads = "Novation Launchkey Mini Pads";
}

TEST_CASE ("MidiProfile parses an optional pad sub-surface", "[plugin][pads][profile]")
{
    bool ok = false;
    auto p = MidiProfile::fromJson (R"({"name":"X","match":["x"],"padChannel":10,"padNotes":[36,51]})", ok);
    REQUIRE (ok);
    REQUIRE (p.hasPadSurface());
    REQUIRE (p.padChannel == 10);
    REQUIRE (p.padLo == 36);
    REQUIRE (p.padHi == 51);

    bool ok2 = false;   // no pad block -> no sub-surface
    auto q = MidiProfile::fromJson (R"({"name":"Y","match":["y"]})", ok2);
    REQUIRE (ok2);
    REQUIRE_FALSE (q.hasPadSurface());
}

TEST_CASE ("only a pad-capable device exposes a '<device> Pads' surface", "[plugin][pads]")
{
    VASynthProcessor p;
    REQUIRE (p.padSubSurfaceName (kLK) == kLKPads);              // Launchkey profile declares pads
    REQUIRE (p.padSubSurfaceName ("Korg B2").isEmpty());        // keyboard, no pads
    REQUIRE (p.padSubSurfaceName ("Generic USB MIDI").isEmpty());
}

TEST_CASE ("pad notes split to the pads surface; keys stay on the device", "[plugin][pads][routing]")
{
    VASynthProcessor p; p.prepareToPlay (kSR, 256);
    p.setSurfaceRouting (kLK,     1);       // keys  -> part 1
    p.setSurfaceRouting (kLKPads, 2);       // pads  -> part 2

    auto render = [&] { juce::AudioBuffer<float> b (2, 256); b.clear(); juce::MidiBuffer m; p.processBlock (b, m); };

    SECTION ("a KEY note (ch1) plays the device's part, not the pads' part")
    {
        const auto k0 = p.partActivity (1), d0 = p.partActivity (2);
        p.routeDeviceMessage (kLK, juce::MidiMessage::noteOn (1, 60, 0.9f));
        render();
        REQUIRE (p.partActivity (1) > k0);           // key -> part 1
        REQUIRE (p.partActivity (2) == d0);          // pads part untouched
    }

    SECTION ("a PAD note (ch10, in range) plays ONLY the pads' part")
    {
        const auto k0 = p.partActivity (1), d0 = p.partActivity (2);
        p.routeDeviceMessage (kLK, juce::MidiMessage::noteOn (10, 36, 0.9f));
        render();
        REQUIRE (p.partActivity (2) > d0);           // pad -> part 2
        REQUIRE (p.partActivity (1) == k0);          // device/keys part untouched (excluded)
    }

    SECTION ("a ch10 note OUTSIDE the pad range is not split (stays on the device)")
    {
        const auto k0 = p.partActivity (1), d0 = p.partActivity (2);
        p.routeDeviceMessage (kLK, juce::MidiMessage::noteOn (10, 72, 0.9f));   // 72 > padHi(51)
        render();
        REQUIRE (p.partActivity (1) > k0);           // -> the device surface -> part 1
        REQUIRE (p.partActivity (2) == d0);
    }
}

TEST_CASE ("a non-pad device is never split (routeDeviceMessage == routeSurfaceMessage)", "[plugin][pads][routing]")
{
    VASynthProcessor p; p.prepareToPlay (kSR, 256);
    p.setSurfaceRouting ("Korg B2", 3);
    const auto h0 = p.partActivity (3);
    p.routeDeviceMessage ("Korg B2", juce::MidiMessage::noteOn (10, 40, 0.9f));  // ch10 but device has no pads
    juce::AudioBuffer<float> b (2, 256); b.clear(); juce::MidiBuffer m; p.processBlock (b, m);
    REQUIRE (p.partActivity (3) > h0);               // whole device (incl ch10) -> its one part
}

TEST_CASE ("pad-surface routing round-trips through a MULTI", "[plugin][pads][multi]")
{
    VASynthProcessor src; src.prepareToPlay (kSR, 256);
    src.setPartPreset (1, "Fat Saw Bass");          // give part 1 a real sound so routing survives
    src.setSurfaceRouting (kLK,     1);             // keys -> the bass part
    src.setSurfaceRouting (kLKPads, 3);             // pads -> P4 (the default-scene 808 kit)
    auto multi = src.captureMultiState();

    VASynthProcessor dst; dst.prepareToPlay (kSR, 256);
    dst.applyMultiState (multi);
    REQUIRE (dst.getSurfaceRouting (kLK)     == 1);
    REQUIRE (dst.getSurfaceRouting (kLKPads) == 3);  // the pads surface persisted independently
}

TEST_CASE ("MIDI monitor records the surface + channel of each message (F12 diagnostic)", "[plugin][pads][monitor]")
{
    VASynthProcessor p; p.prepareToPlay (kSR, 256);
    // A key (ch1) and a pad (ch10) from the same Launchkey take different surfaces.
    p.routeDeviceMessage (kLK, juce::MidiMessage::noteOn (1, 60, 0.9f));
    p.routeDeviceMessage (kLK, juce::MidiMessage::noteOn (10, 36, 0.9f));
    auto lines = p.midiMonitorLines();          // newest first
    REQUIRE (lines.size() >= 2);
    REQUIRE (lines[0].contains (kLKPads));       // the pad landed on the Pads surface
    REQUIRE (lines[0].contains ("ch10"));
    REQUIRE (lines[0].contains ("note 36"));
    REQUIRE (lines[1].contains ("ch1"));
    REQUIRE (lines[1].contains ("note 60"));
    REQUIRE_FALSE (lines[1].contains (kLKPads)); // the key stayed on the device surface
}
