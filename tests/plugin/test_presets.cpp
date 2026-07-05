// ============================================================================
// Preset system: randomize (all params, in range) and save/load round-trip.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginProcessor.h"
#include "PresetManager.h"
#include <vector>

TEST_CASE ("randomize sets every parameter within its range and actually changes them", "[plugin][preset][random]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    PresetManager pm (p.apvts);

    auto& params = p.getParameters();
    std::vector<float> before;
    for (auto* prm : params) before.push_back (prm->getValue());

    juce::Random rng (12345);
    pm.randomize (rng);

    int changed = 0;
    for (int i = 0; i < params.size(); ++i)
    {
        const float v = params[i]->getValue();
        REQUIRE (v >= 0.0f);            // normalized values are always in range
        REQUIRE (v <= 1.0f);
        if (std::abs (v - before[(size_t) i]) > 1e-6f) ++changed;
    }
    // The vast majority should have moved (a shuffle, not a no-op). A handful of
    // params legitimately hold: osc_mix (frozen, skipped), osc1_on (forced on),
    // and low-cardinality choice params that can randomly land on their default.
    REQUIRE (changed >= params.size() * 3 / 4);

    // Master gain kept audible (0.5..0.8) so a random patch is never gain-silent.
    const float mg = p.apvts.getParameter ("master_gain")->getValue();
    REQUIRE (mg >= 0.5f);
    REQUIRE (mg <= 0.8f);
}

TEST_CASE ("save then load round-trips the parameter state", "[plugin][preset][saveload]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    PresetManager pm (p.apvts);

    // Set a distinctive state and save it. (osc2_wave is a choice param, so its
    // normalized value snaps — capture the snapped value to compare against.)
    p.apvts.getParameter ("filter_cutoff")->setValueNotifyingHost (0.42f);
    p.apvts.getParameter ("lfo_rate")->setValueNotifyingHost (0.9f);
    p.apvts.getParameter ("osc2_wave")->setValueNotifyingHost (0.33f);
    const float wave = p.apvts.getParameter ("osc2_wave")->getValue();
    const juce::String name = "unit-test-preset-" + juce::String (juce::Random::getSystemRandom().nextInt (1000000));
    REQUIRE (pm.save (name));
    REQUIRE (pm.getPresetNames().contains (name));

    // Change everything, then load the preset back.
    juce::Random rng (7); pm.randomize (rng);
    REQUIRE (pm.load (name));

    REQUIRE (p.apvts.getParameter ("filter_cutoff")->getValue() == Catch::Approx (0.42f).margin (1e-4));
    REQUIRE (p.apvts.getParameter ("lfo_rate")->getValue()      == Catch::Approx (0.9f).margin (1e-4));
    REQUIRE (p.apvts.getParameter ("osc2_wave")->getValue()     == Catch::Approx (wave).margin (1e-4));

    pm.presetDir().getChildFile (name + ".vasynth").deleteFile();      // cleanup
}

TEST_CASE ("loading a pre-6A preset migrates osc_mix to per-source levels", "[plugin][preset][migration]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    PresetManager pm (p.apvts);

    // Forge a legacy preset on disk: the APVTS state with osc_mix set and the
    // per-source level children stripped, exactly as a patch saved before the
    // level-mixer landed would look. The user's own saved presets are like this.
    p.apvts.getParameter ("osc_mix")->setValueNotifyingHost (0.3f);
    auto tree = p.apvts.copyState();
    for (int i = tree.getNumChildren(); --i >= 0;)
    {
        const auto id = tree.getChild (i).getProperty ("id").toString();
        if (id == "osc1_level" || id == "osc2_level" || id == "osc3_level")
            tree.removeChild (i, nullptr);
    }
    const juce::String name = "legacy-test-" + juce::String (juce::Random::getSystemRandom().nextInt (1000000));
    tree.createXml()->writeTo (pm.presetDir().getChildFile (name + ".vasynth"));

    // Move levels away from where migration should land, then load the legacy file.
    p.apvts.getParameter ("osc1_level")->setValueNotifyingHost (0.123f);
    REQUIRE (pm.load (name));

    // osc1 = 1-mix, osc2 = mix, osc3 off — the old crossfade preserved.
    REQUIRE (p.apvts.getParameter ("osc1_level")->getValue() == Catch::Approx (0.7f).margin (1e-4));
    REQUIRE (p.apvts.getParameter ("osc2_level")->getValue() == Catch::Approx (0.3f).margin (1e-4));
    REQUIRE (p.apvts.getParameter ("osc3_level")->getValue() == Catch::Approx (0.0f).margin (1e-4));

    pm.presetDir().getChildFile (name + ".vasynth").deleteFile();      // cleanup
}
