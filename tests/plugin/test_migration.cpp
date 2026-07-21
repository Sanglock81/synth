// ============================================================================
// 6A state migration: loading a pre-level-model patch (has osc_mix, no
// osc1_level) derives the per-source levels from the legacy crossfade so old
// patches sound unchanged.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginProcessor.h"

TEST_CASE ("legacy osc_mix migrates to per-source levels on load", "[plugin][6a][migration]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // Build a synthetic pre-level state: a fresh APVTS tree with osc_mix set,
    // and the level params stripped out (as an old session/preset would be).
    VASynthProcessor src;
    src.apvts.getParameter ("osc_mix")->setValueNotifyingHost (0.3f);

    auto tree = src.apvts.copyState();
    for (int i = tree.getNumChildren(); --i >= 0;)
    {
        const auto id = tree.getChild (i).getProperty ("id").toString();
        if (id == "osc1_level" || id == "osc2_level" || id == "osc3_level")
            tree.removeChild (i, nullptr);
    }
    auto xml = tree.createXml();
    juce::MemoryBlock blob;
    VASynthProcessor::xmlToBinaryForTest (*xml, blob);

    // Load into a fresh processor -> migration should fire.
    VASynthProcessor dst;
    dst.setStateInformation (blob.getData(), (int) blob.getSize());

    // osc1 = 1-mix, osc2 = mix, osc3 = 0 (levels are 0..1 -> normalized == value).
    REQUIRE (dst.apvts.getParameter ("osc1_level")->getValue() == Catch::Approx (0.7f).margin (1e-4));
    REQUIRE (dst.apvts.getParameter ("osc2_level")->getValue() == Catch::Approx (0.3f).margin (1e-4));
    REQUIRE (dst.apvts.getParameter ("osc3_level")->getValue() == Catch::Approx (0.0f).margin (1e-4));
}

TEST_CASE ("state carrying removed params (eq_*, arp_latch) loads cleanly, values discarded", "[plugin][migration]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // Simulate an OLD session that saved the now-removed master-EQ + arp-latch params.
    VASynthProcessor src;
    src.apvts.getParameter ("filter_cutoff")->setValueNotifyingHost (0.42f);   // a real param that must survive
    auto tree = src.apvts.copyState();
    auto addOld = [&] (const char* id, float v)
    {
        juce::ValueTree p ("PARAM"); p.setProperty ("id", id, nullptr); p.setProperty ("value", v, nullptr);
        tree.appendChild (p, nullptr);
    };
    addOld ("eq_on", 1.0f); addOld ("eq_ls_gain", 12.0f); addOld ("eq_hm_freq", 3000.0f);
    addOld ("eq_hs_gain", -6.0f); addOld ("arp_latch", 1.0f);

    auto xml = tree.createXml();
    juce::MemoryBlock blob;
    VASynthProcessor::xmlToBinaryForTest (*xml, blob);

    // Must not crash, and the unknown IDs must simply be ignored.
    VASynthProcessor dst;
    dst.setStateInformation (blob.getData(), (int) blob.getSize());
    REQUIRE (dst.apvts.getParameter ("eq_on")   == nullptr);   // param no longer exists
    REQUIRE (dst.apvts.getParameter ("arp_latch") == nullptr);
    REQUIRE (dst.apvts.getParameter ("filter_cutoff")->getValue() == Catch::Approx (0.42f).margin (1e-4));  // real param survived
}

TEST_CASE ("modern state with levels round-trips unchanged (no spurious migration)", "[plugin][6a][migration]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor src;
    src.apvts.getParameter ("osc_mix")->setValueNotifyingHost (0.3f);   // legacy present but ignored
    src.apvts.getParameter ("osc1_level")->setValueNotifyingHost (0.42f);
    src.apvts.getParameter ("osc3_level")->setValueNotifyingHost (0.9f);

    juce::MemoryBlock blob;
    src.getStateInformation (blob);

    VASynthProcessor dst;
    dst.setStateInformation (blob.getData(), (int) blob.getSize());
    REQUIRE (dst.apvts.getParameter ("osc1_level")->getValue() == Catch::Approx (0.42f).margin (1e-4));
    REQUIRE (dst.apvts.getParameter ("osc3_level")->getValue() == Catch::Approx (0.9f).margin (1e-4));
}

// ---------------------------------------------------------------------------
// 8A rename migration: an existing rig's config (presets + MIDI profiles) under
// the pre-rename "VASynth" dir is carried over to the new "synth" dir once, and
// re-running is a no-op (never overwrites/duplicates).
#include "AppInfo.h"

TEST_CASE ("config migration copies legacy presets/profiles once (idempotent)", "[plugin][8a][migration]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("synth-migrate-test-" + juce::String (juce::Random::getSystemRandom().nextInt (1'000'000)));
    auto legacy = tmp.getChildFile ("VASynth");
    auto dest   = tmp.getChildFile ("synth");

    // Seed a legacy rig: one user preset + one user MIDI profile.
    legacy.getChildFile ("presets").createDirectory();
    legacy.getChildFile ("midi-profiles").createDirectory();
    legacy.getChildFile ("presets/My Patch.vasynth").replaceWithText ("<preset/>");
    legacy.getChildFile ("midi-profiles/my_ctrl.json").replaceWithText ("{}");

    const int copied = AppInfo::migrateConfigTree (legacy, dest);
    REQUIRE (copied == 2);
    REQUIRE (dest.getChildFile ("presets/My Patch.vasynth").existsAsFile());
    REQUIRE (dest.getChildFile ("midi-profiles/my_ctrl.json").existsAsFile());

    // Idempotent: a second run copies nothing and does not clobber the dest.
    dest.getChildFile ("presets/My Patch.vasynth").replaceWithText ("<edited-after-migration/>");
    const int again = AppInfo::migrateConfigTree (legacy, dest);
    REQUIRE (again == 0);
    REQUIRE (dest.getChildFile ("presets/My Patch.vasynth").loadFileAsString() == "<edited-after-migration/>");

    // No legacy dir -> no-op (fresh machine / CI).
    REQUIRE (AppInfo::migrateConfigTree (tmp.getChildFile ("nonexistent"), dest) == 0);

    tmp.deleteRecursively();
}
