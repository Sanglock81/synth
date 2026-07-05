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
