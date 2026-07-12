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

TEST_CASE ("kit pad edit flow: begin -> edit -> commit bakes into the pad", "[plugin][kit][edit]")
{
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    p.setPartKit (1, onePadKit (60, "Init", {}));
    REQUIRE (padEnergy (p, 1, 60) > 0.0);          // control: the pad is audible

    REQUIRE (p.beginKitPadEdit (1, 0));
    REQUIRE (p.isEditingKitPad());
    REQUIRE (p.isPartKit (1));                     // the kit stays intact; only one pad is live
    for (auto* id : { ParamID::osc1On, ParamID::osc2On, ParamID::osc3On, ParamID::noiseLevel })
        p.apvts.getParameter (id)->setValueNotifyingHost (0.0f);   // edit the voice -> silent
    p.endKitPadEdit (true);                        // commit
    REQUIRE_FALSE (p.isEditingKitPad());
    REQUIRE (p.isPartKit (1));                     // part is a kit again

    // Verify the edit baked in on a FRESH processor (no residual voice from earlier triggers).
    VASynthProcessor q; q.prepareToPlay (48000.0, 128);
    q.setPartKit (1, p.getPartKit (1));            // the edited kit definition
    REQUIRE (q.getPartKit (1).pads[0].voiceState.isValid());
    REQUIRE (padEnergy (q, 1, 60) < 1.0e-4);       // the edited (silent) voice is what plays
}

TEST_CASE ("kit pad edit flow: cancel leaves the pad unchanged", "[plugin][kit][edit]")
{
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    p.setPartKit (1, onePadKit (60, "Init", {}));
    REQUIRE (p.beginKitPadEdit (1, 0));
    for (auto* id : { ParamID::osc1On, ParamID::osc2On, ParamID::osc3On, ParamID::noiseLevel })
        p.apvts.getParameter (id)->setValueNotifyingHost (0.0f);
    p.endKitPadEdit (false);                        // cancel
    REQUIRE (padEnergy (p, 1, 60) > 0.0);          // still audible (edit discarded)
}

TEST_CASE ("kit pad edit refuses an empty pad or a non-kit part", "[plugin][kit][edit]")
{
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    REQUIRE_FALSE (p.beginKitPadEdit (1, 0));       // part 1 isn't a kit yet
    p.setPartKit (1, onePadKit (60, "Init", {}));
    REQUIRE_FALSE (p.beginKitPadEdit (1, 5));       // pad 5 is empty
    REQUIRE_FALSE (p.beginKitPadEdit (0, 0));       // part 0 can't be a kit
}

TEST_CASE ("kit pad edit: the OTHER pads keep playing while one pad is edited", "[plugin][kit][edit]")
{
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    VASynthProcessor::KitDefinition def; def.name = "Two";
    for (int i = 0; i < 2; ++i)
    {
        auto& pd = def.pads[(std::size_t) i];
        pd.triggerNote = 60 + i * 2; pd.source = "Init"; pd.soundNote[0] = 60 + i * 2; pd.numSound = 1; pd.level = 1.0f;
    }
    p.setPartKit (1, def);

    REQUIRE (p.beginKitPadEdit (1, 0));            // edit pad 0
    for (auto* id : { ParamID::osc1On, ParamID::osc2On, ParamID::osc3On, ParamID::noiseLevel })
        p.apvts.getParameter (id)->setValueNotifyingHost (0.0f);   // pad 0's live voice -> silent

    // Pad 0 (edited) reflects the live edit -> silent. Checked first so its (silent) voice
    // can't mask the next measurement.
    REQUIRE (padEnergy (p, 1, 60) < 1.0e-4);
    // Pad 1 keeps its baked sound and plays normally while pad 0 is being edited.
    REQUIRE (padEnergy (p, 1, 62) > 0.0);
    p.endKitPadEdit (false);
}
