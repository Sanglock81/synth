// ============================================================================
// [6d] Factory preset library: all presets present, categorised, each loads and
// actually makes sound, and Init resets to defaults.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginProcessor.h"
#include <cmath>

namespace
{
    constexpr double kSR = 48000.0;
    constexpr int    kBlock = 512;

    // Play middle C for ~2 s (covers slow pad attacks) and return the peak.
    float presetPeak (VASynthProcessor& p)
    {
        p.prepareToPlay (kSR, kBlock);
        float peak = 0.0f;
        const int blocks = (int) (kSR * 2.0 / kBlock);
        for (int b = 0; b < blocks; ++b)
        {
            juce::AudioBuffer<float> buf (2, kBlock);
            buf.clear();
            juce::MidiBuffer midi;
            if (b == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
            p.processBlock (buf, midi);
            peak = std::max (peak, buf.getMagnitude (0, kBlock));
        }
        return peak;
    }
}

TEST_CASE ("factory library has the expected presets spanning the categories", "[plugin][6d][presets]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    const auto& lib = p.factoryPresetLibrary();

    REQUIRE (lib.size() == 22);        // 16 tonal + 6 drums (7A)
    for (auto& fp : lib.all())
    {
        REQUIRE (fp.name.isNotEmpty());
        REQUIRE (fp.category.isNotEmpty());
    }

    auto cats = lib.categories();
    for (auto* expected : { "Bass", "Lead", "Keys", "Pad", "Pluck", "Brass", "Strings", "Winds", "Organ", "FX", "Drums" })
        REQUIRE (cats.contains (expected));
}

TEST_CASE ("every factory preset loads and makes sound", "[plugin][6d][presets]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;

    for (auto& fp : p.factoryPresetLibrary().all())
    {
        p.loadFactoryPreset (fp.name);
        INFO ("preset: " << fp.name);
        REQUIRE (presetPeak (p) > 0.005f);      // audible, not a dead patch
    }
}

TEST_CASE ("loadFactoryPreset applies overrides; Init resets to defaults", "[plugin][6d][presets][init]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;

    p.loadFactoryPreset ("Warm Pad");
    REQUIRE (p.apvts.getRawParameterValue ("fx_reverb_on")->load() > 0.5f);   // pad enables reverb
    REQUIRE (p.apvts.getRawParameterValue ("amp_attack")->load()   > 0.3f);   // slow attack

    p.loadInitPreset();
    REQUIRE (p.apvts.getRawParameterValue ("fx_reverb_on")->load() < 0.5f);   // back to default (off)
    REQUIRE (p.apvts.getRawParameterValue ("amp_attack")->load() == Catch::Approx (0.005f).margin (1e-4));
    int order[5]; p.getFxOrder (order);
    REQUIRE (order[0] == 3); REQUIRE (order[1] == 0);                          // default chain order: WIDTH first
}

TEST_CASE ("a SOUND preset does not rearrange the FX chain order", "[plugin][6d][presets][order]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    // The chain order is a global, user-controlled setting (reordered via the panel chevrons). No
    // factory preset carries an fxOrder, so loading one must leave whatever order the user set — the
    // previous fallback to {0,1,2,3,4} snapped WIDTH out of first place on every order-less preset.
    const int userOrder[5] { 1, 3, 0, 2, 4 };
    p.setFxOrder (userOrder);
    p.loadFactoryPreset ("Warm Pad");
    int order[5]; p.getFxOrder (order);
    for (int i = 0; i < 5; ++i) REQUIRE (order[i] == userOrder[i]);   // untouched by the sound load
}
