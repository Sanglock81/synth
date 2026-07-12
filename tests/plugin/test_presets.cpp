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

    const auto& soundIds = VASynthProcessor::soundDesignParamIDs();
    auto& params = p.getParameters();
    std::vector<float> before;
    for (auto* prm : params) before.push_back (prm->getValue());

    juce::Random rng (12345);
    pm.randomize (rng, soundIds);

    int changedSound = 0, soundCount = 0, changedOther = 0;
    for (int i = 0; i < params.size(); ++i)
    {
        const float v = params[i]->getValue();
        REQUIRE (v >= 0.0f);            // normalized values are always in range
        REQUIRE (v <= 1.0f);
        auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (params[i]);
        const juce::String id = withId != nullptr ? withId->paramID : juce::String();
        const bool isSound = soundIds.contains (id) && ! PresetManager::randomizeExclusions().contains (id);
        const bool moved = std::abs (v - before[(size_t) i]) > 1e-6f;
        if (isSound) ++soundCount;
        if (moved) { if (isSound) ++changedSound; else ++changedOther; }
    }
    // ONLY sound-design params move — the mixer, EQ, macros and every global stay put.
    REQUIRE (changedOther == 0);
    // ...and the majority of the sound-design set actually shuffled (not a no-op). Held:
    // osc1_on (forced on) + low-cardinality choice params that can land on their default.
    REQUIRE (changedSound >= soundCount / 2);
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
        { ParamID::chordEnabled,1.0f  },
        { ParamID::chordRoot,   0.5f  },
        { ParamID::chordScale,  1.0f  },
        { ParamID::oscMix,      0.9f  },
        // Rhythm section (R3): arp / sequencer / looper + shared tempo are performance
        // state Random must never touch.
        { ParamID::tempo,       0.42f },
        { ParamID::arpOn,       1.0f  },
        { ParamID::arpMode,     0.5f  },
        { ParamID::arpOctaves,  0.66f },
        { ParamID::arpGate,     0.4f  },
        { ParamID::arpSwing,    0.3f  },
        { ParamID::arpLatch,    1.0f  },
        { ParamID::arpHold,     1.0f  },
        { ParamID::loopRec,     1.0f  },
        { ParamID::loopPlay,    1.0f  },
        { ParamID::loopBars,    0.5f  },
        { ParamID::loopMode,    1.0f  },   // MIDI/AUDIO playback lane (Group 3) — performance state
    };
    for (auto& kv : pinned) p.apvts.getParameter (kv.id)->setValueNotifyingHost (kv.norm);
    std::vector<float> want;
    for (auto& kv : pinned) want.push_back (p.apvts.getParameter (kv.id)->getValue());

    // Hammer randomize many times — an exclusion leak would show up statistically.
    for (int i = 0; i < 200; ++i) { juce::Random rng (i * 2654435761u + 1u); pm.randomize (rng, VASynthProcessor::soundDesignParamIDs()); }

    for (size_t i = 0; i < pinned.size(); ++i)
        REQUIRE (p.apvts.getParameter (pinned[i].id)->getValue() == Catch::Approx (want[i]).margin (1e-6));

    // Sanity: the policy list is exactly this pinned set (a single visible source).
    REQUIRE (PresetManager::randomizeExclusions().size() == (int) pinned.size());
    for (auto& kv : pinned)
        REQUIRE (PresetManager::randomizeExclusions().contains (kv.id));
}

TEST_CASE ("randomize touches ONLY the selected part sound - mixer / EQ / macros stay put",
           "[plugin][preset][random]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    PresetManager pm (p.apvts);

    // Cross-part mix + master controls the player has dialled in — Random must not disturb
    // these (they span every part, which is what "affects every part" was about).
    struct KV { const char* id; float norm; };
    const std::vector<KV> mix {
        { ParamID::part0Level, 0.7f }, { ParamID::part0Pan, 0.3f },
        { ParamID::part1Level, 0.4f }, { ParamID::part1Pan, 0.8f },
        { ParamID::part2Level, 0.9f }, { ParamID::part3Pan, 0.2f },
        { ParamID::eqLmGain,   0.75f }, { ParamID::eqHmFreq, 0.6f }, { ParamID::eqOn, 1.0f },
        { ParamID::macro1, 0.25f }, { ParamID::macro4, 0.65f }, { ParamID::macro8, 0.5f },
    };
    for (auto& kv : mix) p.apvts.getParameter (kv.id)->setValueNotifyingHost (kv.norm);
    std::vector<float> want;
    for (auto& kv : mix) want.push_back (p.apvts.getParameter (kv.id)->getValue());
    const float cutoffBefore = p.apvts.getParameter (ParamID::filterCutoff)->getValue();

    for (int i = 0; i < 100; ++i)
    { juce::Random rng (i * 40503u + 7u); pm.randomize (rng, VASynthProcessor::soundDesignParamIDs()); }

    for (size_t i = 0; i < mix.size(); ++i)
        REQUIRE (p.apvts.getParameter (mix[i].id)->getValue() == Catch::Approx (want[i]).margin (1e-6));

    // ...but a sound-design param DID move (proves randomize actually ran).
    REQUIRE (p.apvts.getParameter (ParamID::filterCutoff)->getValue() != Catch::Approx (cutoffBefore).margin (1e-6));
}

TEST_CASE ("numeric entry: typed text parses back onto the parameter", "[plugin][ui][numeric]")
{
    // The double-click numeric entry sets a param via getValueForText(typed) — guard that
    // the parse round-trips for a skewed (Hz) and a plain param.
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;

    auto* cutoff = p.apvts.getParameter (ParamID::filterCutoff);
    cutoff->setValueNotifyingHost (cutoff->getValueForText ("2000"));
    REQUIRE (p.apvts.getRawParameterValue (ParamID::filterCutoff)->load() == Catch::Approx (2000.0f).margin (60.0f));

    auto* atk = p.apvts.getParameter (ParamID::ampAttack);
    atk->setValueNotifyingHost (atk->getValueForText ("0.5"));
    REQUIRE (p.apvts.getRawParameterValue (ParamID::ampAttack)->load() == Catch::Approx (0.5f).margin (0.05f));
}

TEST_CASE ("CLEAR blanks the selected part to a single sine, leaving globals put", "[plugin][clear]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    // A busy sound + dialled-in globals the player wants kept.
    auto set = [&] (const char* id, float v) { p.apvts.getParameter (id)->setValueNotifyingHost (v); };
    set (ParamID::osc2On, 1.0f); set (ParamID::osc3On, 1.0f); set (ParamID::noiseLevel, 0.4f);
    set (ParamID::fxReverbOn, 1.0f); set (ParamID::fxDelayOn, 1.0f);
    set (ParamID::masterGain, 0.42f); set (ParamID::part0Level, 0.7f); set (ParamID::macro3, 0.8f);
    const float masterBefore = p.apvts.getParameter (ParamID::masterGain)->getValue();
    const float levelBefore  = p.apvts.getParameter (ParamID::part0Level)->getValue();
    const float macroBefore  = p.apvts.getParameter (ParamID::macro3)->getValue();

    p.clearFocusedPartToBlank();

    // A clean single sine: osc1 on + sine (last choice), everything else off.
    REQUIRE (p.apvts.getRawParameterValue (ParamID::osc1On)->load() > 0.5f);
    REQUIRE (p.apvts.getRawParameterValue (ParamID::osc2On)->load() < 0.5f);
    REQUIRE (p.apvts.getRawParameterValue (ParamID::osc3On)->load() < 0.5f);
    REQUIRE (p.apvts.getRawParameterValue (ParamID::noiseLevel)->load() < 1e-4f);
    REQUIRE (p.apvts.getRawParameterValue (ParamID::fxReverbOn)->load() < 0.5f);
    REQUIRE (p.apvts.getRawParameterValue (ParamID::fxDelayOn)->load() < 0.5f);
    REQUIRE (p.apvts.getParameter (ParamID::osc1Wave)->getValue() == Catch::Approx (1.0f).margin (1e-4));
    // Globals / mixer / macros untouched (same scope as RANDOM).
    REQUIRE (p.apvts.getParameter (ParamID::masterGain)->getValue()  == Catch::Approx (masterBefore).margin (1e-6));
    REQUIRE (p.apvts.getParameter (ParamID::part0Level)->getValue() == Catch::Approx (levelBefore).margin (1e-6));
    REQUIRE (p.apvts.getParameter (ParamID::macro3)->getValue()     == Catch::Approx (macroBefore).margin (1e-6));
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
    juce::Random rng (7); pm.randomize (rng, VASynthProcessor::soundDesignParamIDs());
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
