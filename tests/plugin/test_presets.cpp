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
    // Nearly all parameters should have moved (a shuffle, not a no-op).
    REQUIRE (changed >= params.size() - 2);

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
