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
