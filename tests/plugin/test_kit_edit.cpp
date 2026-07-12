// ============================================================================
// Kit per-pad voice editing (Group 4 increment A): a pad can carry its own edited
// voice state, which setPartKit bakes instead of the source preset, and which
// round-trips through kit serialization (kit files + MULTIs).
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "PluginProcessor.h"

namespace
{
    // Energy of a kit pad triggered on `kitPart` (routed via play-focus).
    double padEnergy (VASynthProcessor& p, int kitPart, int trigNote, int blocks = 40)
    {
        p.setEditFocus (kitPart);                    // route the live keyboard to the kit part
        p.routeNoteOn (trigNote, 0.9f, 0);           // surface 0 -> play-focus (the kit)
        juce::AudioBuffer<float> buf (2, 128); juce::MidiBuffer m; double e = 0.0;
        for (int b = 0; b < blocks; ++b) { buf.clear(); p.processBlock (buf, m); e += buf.getRMSLevel (0, 0, 128); }
        return e;
    }

    juce::ValueTree silentVoiceState()
    {
        VASynthProcessor tmp;
        tmp.loadInitPreset();
        for (auto* id : { ParamID::osc1On, ParamID::osc2On, ParamID::osc3On, ParamID::noiseLevel })
            tmp.apvts.getParameter (id)->setValueNotifyingHost (0.0f);   // no sources -> silent voice
        return tmp.apvts.copyState();
    }

    VASynthProcessor::KitDefinition onePadKit (int trig, const juce::String& source, juce::ValueTree voice)
    {
        VASynthProcessor::KitDefinition def; def.name = "TestKit";
        auto& pad = def.pads[0];
        pad.triggerNote = trig; pad.source = source; pad.soundNote[0] = trig; pad.numSound = 1; pad.level = 1.0f;
        pad.voiceState = voice;
        return def;
    }
}

TEST_CASE ("kit pad bakes from its voice-state override, not the source preset", "[plugin][kit][edit]")
{
    // Control: source "Init" with no override -> audible.
    {
        VASynthProcessor p; p.prepareToPlay (48000.0, 128);
        p.setPartKit (1, onePadKit (60, "Init", {}));
        REQUIRE (padEnergy (p, 1, 60) > 0.0);
    }
    // Override with a SILENT voice -> the pad is silent (the override baked, not "Init").
    {
        VASynthProcessor p; p.prepareToPlay (48000.0, 128);
        p.setPartKit (1, onePadKit (60, "Init", silentVoiceState()));
        REQUIRE (padEnergy (p, 1, 60) < 1.0e-4);
    }
}

TEST_CASE ("kit pad voice-state round-trips through kit serialization", "[plugin][kit][edit][state]")
{
    auto def = onePadKit (60, "Init", silentVoiceState());
    REQUIRE (def.pads[0].voiceState.isValid());

    auto tree = VASynthProcessor::kitToTree (def);
    auto back = VASynthProcessor::kitFromTree (tree);
    REQUIRE (back.pads[0].voiceState.isValid());

    // The restored override still bakes to a silent pad (the edited voice survived).
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    p.setPartKit (1, back);
    REQUIRE (padEnergy (p, 1, 60) < 1.0e-4);
}
