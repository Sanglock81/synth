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
    // The majority of SOUND-DESIGN params should move (a shuffle, not a no-op).
    // Held: the performance/global exclusion list, osc1_on (forced on), and
    // low-cardinality choice params that can randomly land on their default.
    REQUIRE (changed >= params.size() / 2);
}

TEST_CASE ("randomize never touches the performance/global exclusion list", "[plugin][preset][random][bug5]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    PresetManager pm (p.apvts);

    // Set every excluded param to a distinctive value a player might have dialled
    // in (and which is NOT its default), then prove randomize leaves them exactly.
    struct KV { const char* id; float norm; };
    const std::vector<KV> pinned {
        { ParamID::masterGain,  0.42f },
        { ParamID::velToAmp,    0.11f },
        { ParamID::velToCutoff, 0.66f },
        { ParamID::polyMode,    1.0f  },   // Mono
        { ParamID::glideTime,   0.33f },
        { ParamID::oscMix,      0.9f  },
    };
    for (auto& kv : pinned) p.apvts.getParameter (kv.id)->setValueNotifyingHost (kv.norm);
    std::vector<float> want;
    for (auto& kv : pinned) want.push_back (p.apvts.getParameter (kv.id)->getValue());

    // Hammer randomize many times — an exclusion leak would show up statistically.
    for (int i = 0; i < 200; ++i) { juce::Random rng (i * 2654435761u + 1u); pm.randomize (rng); }

    for (size_t i = 0; i < pinned.size(); ++i)
        REQUIRE (p.apvts.getParameter (pinned[i].id)->getValue() == Catch::Approx (want[i]).margin (1e-6));

    // Sanity: the policy list is exactly these six (a single visible source).
    REQUIRE (PresetManager::randomizeExclusions().size() == (int) pinned.size());
    for (auto& kv : pinned)
        REQUIRE (PresetManager::randomizeExclusions().contains (kv.id));
}

TEST_CASE ("master_gain is a performance control excluded from preset load/save", "[plugin][preset][master]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    PresetManager pm (p.apvts);

    auto* mg = p.apvts.getParameter ("master_gain");
    auto setMaster = [&] (float v) { mg->setValueNotifyingHost (v); };

    setMaster (0.42f);                                   // player dials in a level
    p.loadFactoryPreset ("Fat Saw Bass");
    REQUIRE (mg->getValue() == Catch::Approx (0.42f).margin (1e-4));   // factory load keeps it
    p.loadInitPreset();
    REQUIRE (mg->getValue() == Catch::Approx (0.42f).margin (1e-4));   // Init keeps it

    // Save at one master, then load at a different one: the CURRENT master persists.
    setMaster (0.90f);
    const auto name = "ut-master-" + juce::String (juce::Random::getSystemRandom().nextInt (1'000'000));
    REQUIRE (pm.save (name));
    setMaster (0.30f);
    REQUIRE (pm.load (name));
    REQUIRE (mg->getValue() == Catch::Approx (0.30f).margin (1e-4));   // not the saved 0.90

    // The saved file carries no master_gain node at all.
    auto xml = juce::XmlDocument::parse (pm.presetDir().getChildFile (name + ".vasynth"));
    REQUIRE (xml != nullptr);
    bool hasMaster = false;
    for (auto* c : xml->getChildIterator())
        if (c->getStringAttribute ("id") == "master_gain") hasMaster = true;
    REQUIRE_FALSE (hasMaster);

    pm.presetDir().getChildFile (name + ".vasynth").deleteFile();
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
