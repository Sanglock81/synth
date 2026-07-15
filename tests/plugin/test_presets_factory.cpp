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
    REQUIRE (order[0] == 0); REQUIRE (order[3] == 3);                          // default chain order
}

TEST_CASE ("a preset with an fxOrder sets the chain order", "[plugin][6d][presets][order]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    // Warm Pad declares "fxOrder":[0,1,2,3]; load a reordered state first to prove
    // the preset actively sets it.
    const int scrambled[5] { 4, 3, 2, 1, 0 };
    p.setFxOrder (scrambled);
    p.loadFactoryPreset ("Warm Pad");
    int order[5]; p.getFxOrder (order);
    REQUIRE (order[0] == 0); REQUIRE (order[1] == 1); REQUIRE (order[2] == 2); REQUIRE (order[3] == 3);
}
