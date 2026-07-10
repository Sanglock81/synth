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
#include <functional>
#include <cmath>

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

// Largest sample-to-sample jump per channel (a click/pop detector) + bounds + finite,
// over the REAL stereo multi-part -> mixer -> master -> soft-clip topology.
TEST_CASE ("multi-part pedaled play stays clean across silence/return (static regression)", "[plugin][bounds][click][multi]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);

    // Part 0 (live) = Warm Pad (chorus+reverb+width). Part 1 = locked Warm Pad. Part 2 = a kit.
    p.loadFactoryPreset ("Warm Pad");
    p.setPartPreset (1, "Warm Pad");
    p.setPartKit (2, p.loadKit ("808 Basics"));
    auto set = [&p] (const char* id, float v) { p.apvts.getParameter (id)->setValueNotifyingHost (p.apvts.getParameter (id)->convertTo0to1 (v)); };
    set (ParamID::part1Level, 1.4f);   // exercise the mixer level
    set (ParamID::part2Pan, -0.6f);    // and pan

    juce::AudioBuffer<float> buf (2, 128);
    float prevL = 0.0f, prevR = 0.0f, maxJump = 0.0f, peak = 0.0f;
    bool finite = true;
    auto pump = [&] (int blocks, std::function<void(juce::MidiBuffer&, int)> fill)
    {
        for (int b = 0; b < blocks; ++b)
        {
            juce::MidiBuffer m; if (fill) fill (m, b);
            buf.clear(); p.processBlock (buf, m);
            const float* L = buf.getReadPointer (0); const float* R = buf.getReadPointer (1);
            for (int i = 0; i < 128; ++i)
            {
                finite = finite && std::isfinite (L[i]) && std::isfinite (R[i]);
                peak = std::max ({ peak, std::abs (L[i]), std::abs (R[i]) });
                maxJump = std::max ({ maxJump, std::abs (L[i] - prevL), std::abs (R[i] - prevR) });
                prevL = L[i]; prevR = R[i];
            }
        }
    };

    // Warm up smoothers.
    pump (10, {});
    // Pedaled multi-part: chord on all three parts, hold, release (parts go silent + FX
    // tails decay past the skip hold), then RETRIGGER — repeat many cycles.
    for (int cycle = 0; cycle < 6; ++cycle)
    {
        pump (1, [] (juce::MidiBuffer& m, int) { m.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0); });        // part 0 (host->live)
        p.routeMidi (juce::MidiMessage::noteOn (1, 55, 0.9f), 1);      // part 1
        p.routeMidi (juce::MidiMessage::noteOn (1, 36, 1.0f), 2);      // part 2 kit
        pump (40, {});                                                  // hold
        pump (1, [] (juce::MidiBuffer& m, int) { m.addEvent (juce::MidiMessage::noteOff (1, 60), 0); });
        p.routeMidi (juce::MidiMessage::noteOff (1, 55), 1);
        p.routeMidi (juce::MidiMessage::noteOff (1, 36), 2);
        pump (700, {});                                                 // silence: FX tails decay, parts get skipped
    }

    INFO ("peak=" << peak << " maxJump=" << maxJump);
    REQUIRE (finite);
    REQUIRE (peak <= 1.0f);
    REQUIRE (maxJump < 0.35f);          // no discontinuity (static/pop) across the whole run
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
