// ============================================================================
// Plugin-layer: MIDI-learn behaviour, driven end-to-end through processBlock.
//   * a mapped CC moves its target parameter,
//   * learn-mode binds a new CC,
//   * mappings survive a state round-trip (needs the persistence feature —
//     fails until Phase 3 implements it).
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginProcessor.h"

namespace
{
    // Send a single CC through processBlock.
    void sendCC (VASynthProcessor& p, int cc, int value, int channel = 1)
    {
        juce::AudioBuffer<float> audio (2, 64);
        audio.clear();
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::controllerEvent (channel, cc, value), 0);
        p.processBlock (audio, midi);
    }

    float paramValue (VASynthProcessor& p, const juce::String& id)
    {
        return p.apvts.getParameter (id)->getValue();   // normalized 0..1
    }
}

TEST_CASE ("mapped CC (Launchkey default) moves its target parameter", "[plugin][midilearn]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 64);

    // CC 21 is the default map for macro1 (the Launchkey pots drive the 8 macros).
    sendCC (p, 21, 127);
    REQUIRE (paramValue (p, "macro1") == Catch::Approx (1.0f).margin (1e-3));

    sendCC (p, 21, 0);
    REQUIRE (paramValue (p, "macro1") == Catch::Approx (0.0f).margin (1e-3));
}

// #140: "macro 1 does not auto-link to the Launchkey; the others do." Prove the CC->macro map
// has NO per-macro asymmetry: every Launchkey pot (CC 21-28) drives ONLY its own macro (1-8),
// macro1 included. If this ever failed for one macro it would catch the reported symptom in code;
// it passes, so a knob-1 miss is device-side (the pot emitting a CC other than 21) — use the G1.2
// CC trace (VASYNTH_MIDI_TRACE=1) to see what the controller actually sends.
TEST_CASE ("every Launchkey pot CC21-28 drives only its own macro, macro1 included (#140)", "[plugin][midilearn]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 64);

    for (int m = 0; m < 8; ++m)
    {
        const int cc = 21 + m;
        const juce::String target = "macro" + juce::String (m + 1);
        sendCC (p, cc, 127);
        REQUIRE (paramValue (p, target) == Catch::Approx (1.0f).margin (1e-3));   // this pot moved its macro
        for (int other = 0; other < 8; ++other)                                   // and no other macro
            if (other != m)
                REQUIRE (paramValue (p, "macro" + juce::String (other + 1)) == Catch::Approx (0.0f).margin (1e-3));
        sendCC (p, cc, 0);                                                        // reset for the next pot
    }
}

TEST_CASE ("Reset MIDI restores the Launchkey macro pots after a stale learn", "[plugin][midilearn]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 64);

    // Reproduce the reported problem: a Launchkey pot (CC 21) got learned onto a synth param,
    // so the physical knob drives THAT param and no longer the macro.
    p.getMidiLearn().armLearn (ParamID::filterCutoff);
    sendCC (p, 21, 100);
    REQUIRE (paramValue (p, ParamID::filterCutoff) == Catch::Approx (100.0f / 127.0f).margin (1e-3));

    // Reset MIDI -> CC 21 drives macro1 again, and no longer touches the cutoff.
    p.resetMidiMappings();
    const float cutoffBefore = paramValue (p, ParamID::filterCutoff);
    sendCC (p, 21, 127);
    REQUIRE (paramValue (p, "macro1") == Catch::Approx (1.0f).margin (1e-3));
    REQUIRE (paramValue (p, ParamID::filterCutoff) == Catch::Approx (cutoffBefore).margin (1e-3));   // pot no longer hits cutoff
}

TEST_CASE ("learn mode binds a new CC to a parameter", "[plugin][midilearn]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 64);

    p.getMidiLearn().armLearn ("osc_mix");
    sendCC (p, 50, 64);                                   // unmapped CC -> gets bound
    REQUIRE (paramValue (p, "osc_mix") == Catch::Approx (64.0f / 127.0f).margin (1e-3));

    // Now CC 50 is persistently bound: a new value moves it again.
    sendCC (p, 50, 127);
    REQUIRE (paramValue (p, "osc_mix") == Catch::Approx (1.0f).margin (1e-3));
}

TEST_CASE ("learned mappings survive a state round-trip", "[plugin][midilearn][persist]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    VASynthProcessor src;
    src.prepareToPlay (48000.0, 64);
    src.getMidiLearn().armLearn ("osc_mix");
    sendCC (src, 50, 32);                                 // bind CC 50 -> osc_mix

    juce::MemoryBlock blob;
    src.getStateInformation (blob);

    VASynthProcessor dst;
    dst.prepareToPlay (48000.0, 64);
    dst.setStateInformation (blob.getData(), (int) blob.getSize());

    // On the restored processor, CC 50 should already drive osc_mix.
    sendCC (dst, 50, 100);
    REQUIRE (paramValue (dst, "osc_mix") == Catch::Approx (100.0f / 127.0f).margin (1e-3));
}

TEST_CASE ("profile mapping precedence: learned > user > factory", "[plugin][6c][midilearn][precedence]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    auto& learn = p.getMidiLearn();
    using S = MidiLearnManager::Source;

    // CC 21 starts as a factory default (macro1).
    REQUIRE (learn.getCCForParam ("macro1") == 21);
    REQUIRE (learn.getSource (21) == S::Factory);

    // A user profile overrides factory on the same CC.
    REQUIRE (learn.applyProfileMapping (21, "reverb_mix", S::User));
    REQUIRE (learn.getCCForParam ("reverb_mix") == 21);
    REQUIRE (learn.getSource (21) == S::User);

    // Factory can no longer overwrite the user mapping.
    REQUIRE_FALSE (learn.applyProfileMapping (21, "filter_cutoff", S::Factory));
    REQUIRE (learn.getCCForParam ("reverb_mix") == 21);

    // The user explicitly learns CC 21 -> a new param: learned wins over user.
    p.prepareToPlay (48000.0, 64);
    learn.armLearn ("delay_mix");
    sendCC (p, 21, 64);
    REQUIRE (learn.getCCForParam ("delay_mix") == 21);
    REQUIRE (learn.getSource (21) == S::Learned);

    // Neither a user nor a factory profile can dislodge a learned mapping.
    REQUIRE_FALSE (learn.applyProfileMapping (21, "reverb_mix", S::User));
    REQUIRE_FALSE (learn.applyProfileMapping (21, "filter_cutoff", S::Factory));
    REQUIRE (learn.getCCForParam ("delay_mix") == 21);
}
