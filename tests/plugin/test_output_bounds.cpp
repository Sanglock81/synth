// ============================================================================
// Bug 4 invariant: the processor's final output must NEVER exceed +/-1.0, for
// any patch and any polyphony. This bug shipped because the golden WAVs are
// float and happily stored >1.0 samples — the tests were green while the DAC
// clipped live. This test closes that class of bug: it drives the FULL processor
// (engine trim -> FX -> master gain -> safety clipper) with an adversarial patch
// and asserts every emitted sample is in range.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "PluginProcessor.h"

namespace
{
    void setParam (VASynthProcessor& p, const char* id, float value01)
    {
        if (auto* param = p.apvts.getParameter (id))
            param->setValueNotifyingHost (value01);
    }

    // Worst case for headroom: everything at maximum, all FX engaged.
    void makeAdversarialPatch (VASynthProcessor& p)
    {
        namespace ID = ParamID;
        setParam (p, ID::osc1On, 1.0f); setParam (p, ID::osc2On, 1.0f); setParam (p, ID::osc3On, 1.0f);
        setParam (p, ID::osc1Level, 1.0f); setParam (p, ID::osc2Level, 1.0f); setParam (p, ID::osc3Level, 1.0f);
        setParam (p, ID::noiseLevel, 1.0f);
        setParam (p, ID::filterCutoff, 1.0f);      // wide open -> full harmonic content
        setParam (p, ID::filterReso, 1.0f);        // maximum resonance
        setParam (p, ID::masterGain, 1.0f);        // full master
        setParam (p, ID::fxChorusOn, 1.0f);
        setParam (p, ID::fxDelayOn, 1.0f);
        setParam (p, ID::fxReverbOn, 1.0f);
        setParam (p, ID::fxWidthOn, 1.0f);
        setParam (p, ID::delayFeedback, 1.0f);
        setParam (p, ID::stereoWidth, 1.0f);       // normalized 1.0 -> width 2.0 (max)
    }

    // Largest absolute output sample across `blocks` blocks while holding `nNotes`
    // notes at maximum velocity.
    float maxOutputOverBlocks (VASynthProcessor& p, int nNotes, int blocks, int blockSize)
    {
        juce::AudioBuffer<float> buf (2, blockSize);
        float worst = 0.0f;

        // Note-on all notes in the first block.
        {
            juce::MidiBuffer midi;
            for (int i = 0; i < nNotes; ++i)
                midi.addEvent (juce::MidiMessage::noteOn (1, 36 + i * 3, 1.0f), i);
            buf.clear();
            p.processBlock (buf, midi);
            worst = std::max (worst, buf.getMagnitude (0, buf.getNumSamples()));
        }
        // Sustain for the rest (delay/reverb tails build up here).
        for (int b = 1; b < blocks; ++b)
        {
            juce::MidiBuffer midi;
            buf.clear();
            p.processBlock (buf, midi);
            worst = std::max (worst, buf.getMagnitude (0, buf.getNumSamples()));
        }
        return worst;
    }
}

TEST_CASE ("processor output never exceeds +/-1.0 (adversarial 16-voice chord)", "[plugin][bug4][bounds]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 256);
    makeAdversarialPatch (p);

    // Ramp master smoothing to target before measuring, then hammer it.
    const float worst = maxOutputOverBlocks (p, 16, 400, 256);
    INFO ("worst |output| = " << worst);
    REQUIRE (worst <= 1.0f);
}

TEST_CASE ("processor output stays bounded across all factory presets", "[plugin][bug4][bounds][presets]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 256);

    const auto& lib = p.factoryPresetLibrary();
    for (const auto& preset : lib.all())
    {
        const auto name = preset.name;
        p.loadFactoryPreset (name);
        // Push master to full to expose any preset that rides hot.
        setParam (p, ParamID::masterGain, 1.0f);
        const float worst = maxOutputOverBlocks (p, 12, 200, 256);
        INFO ("preset '" << name << "' worst |output| = " << worst);
        REQUIRE (worst <= 1.0f);
    }
}
