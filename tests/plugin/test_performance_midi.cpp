// ============================================================================
// Pitch bend, mod-wheel vibrato, and sustain pedal — driven with synthetic MIDI
// through processBlock. These arrive from the Launchkey touch strips (bend/mod)
// and the Korg B2 damper (CC64), and must work regardless of source device.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginProcessor.h"
#include "../dsp/test_util.h"
#include <vector>
#include <cmath>

namespace
{
    constexpr double kSR = 48000.0;

    // A processor set to a clean sine osc for easy pitch/level measurement.
    struct Fixture
    {
        juce::ScopedJuceInitialiser_GUI juceInit;
        VASynthProcessor p;
        Fixture()
        {
            p.apvts.getParameter ("osc1_wave")->setValueNotifyingHost (1.0f);  // Sine (last choice)
            p.apvts.getParameter ("osc_mix")->setValueNotifyingHost (0.0f);    // osc1 only
            p.apvts.getParameter ("filter_cutoff")->setValueNotifyingHost (1.0f);
            p.apvts.getParameter ("amp_attack")->setValueNotifyingHost (0.0f);
            p.apvts.getParameter ("amp_sustain")->setValueNotifyingHost (1.0f);
            p.prepareToPlay (kSR, 512);
        }

        // Render `blocks` blocks of 512, optionally injecting one MIDI message at
        // block `atBlock`. Returns concatenated channel-0 output.
        std::vector<float> run (int blocks, juce::MidiMessage msg, int atBlock)
        {
            std::vector<float> out;
            for (int b = 0; b < blocks; ++b)
            {
                juce::AudioBuffer<float> buf (2, 512);
                buf.clear();
                juce::MidiBuffer midi;
                if (b == atBlock) midi.addEvent (msg, 0);
                p.processBlock (buf, midi);
                const float* ch = buf.getReadPointer (0);
                for (int i = 0; i < 512; ++i) out.push_back (ch[i]);
            }
            return out;
        }
    };

    // Fundamental frequency via zero-up-crossing period over a window.
    double estimateHz (const std::vector<float>& x, int start, int len)
    {
        double firstX = -1, lastX = -1; int count = 0; float prev = x[(size_t) start];
        for (int i = start + 1; i < start + len; ++i)
        {
            float v = x[(size_t) i];
            if (prev < 0.0f && v >= 0.0f)
            {
                double frac = double (-prev) / double (v - prev);
                double xh = double (i - 1) + frac;
                if (firstX < 0) firstX = xh; else lastX = xh;
                ++count;
            }
            prev = v;
        }
        if (count < 3) return 0.0;
        return (count - 1) / ((lastX - firstX) / kSR);
    }

    double rms (const std::vector<float>& x, int start, int len)
    {
        double a = 0; for (int i = 0; i < len; ++i) { float s = x[(size_t)(start + i)]; a += double (s) * s; }
        return std::sqrt (a / len);
    }
}

TEST_CASE ("pitch bend shifts pitch by +/- 2 semitones", "[plugin][midi][bend]")
{
    Fixture f;
    auto noteOn = juce::MidiMessage::noteOn (1, 69, 0.9f);                    // A4 = 440 Hz

    // baseline: note on at block 0, no bend
    auto base = f.run (24, noteOn, 0);
    const double f0 = estimateHz (base, 512 * 8, 512 * 8);

    // bent up: fresh fixture, note on then full up-bend
    Fixture g;
    { juce::AudioBuffer<float> b (2, 512); b.clear(); juce::MidiBuffer m;
      m.addEvent (juce::MidiMessage::noteOn (1, 69, 0.9f), 0); g.p.processBlock (b, m); }
    auto bent = g.run (24, juce::MidiMessage::pitchWheel (1, 16383), 0);
    const double f1 = estimateHz (bent, 512 * 12, 512 * 8);

    INFO ("f0=" << f0 << " f1=" << f1 << " ratio=" << f1 / f0);
    REQUIRE (f0 == Catch::Approx (440.0).margin (6.0));
    REQUIRE (f1 / f0 == Catch::Approx (std::pow (2.0, 2.0 / 12.0)).epsilon (0.02));  // +2 semis
}

TEST_CASE ("mod wheel adds vibrato (pitch wavers)", "[plugin][midi][modwheel]")
{
    // With mod wheel up, the instantaneous pitch should oscillate; measure the
    // spread of per-cycle frequency across the note vs. mod wheel down.
    auto pitchSpread = [](bool modUp)
    {
        Fixture f;
        { juce::AudioBuffer<float> b (2, 512); b.clear(); juce::MidiBuffer m;
          m.addEvent (juce::MidiMessage::noteOn (1, 69, 0.9f), 0);
          if (modUp) m.addEvent (juce::MidiMessage::controllerEvent (1, 1, 127), 0);
          f.p.processBlock (b, m); }
        auto out = f.run (60, juce::MidiMessage::noteOff (1, 69), 999);   // hold, no event
        // measure Hz in successive short windows across ~0.5 s
        double lo = 1e9, hi = 0;
        for (int w = 4; w < 55; ++w)
        {
            double hz = estimateHz (out, 512 * w, 512);
            if (hz > 100 && hz < 2000) { lo = std::min (lo, hz); hi = std::max (hi, hz); }
        }
        return hi - lo;
    };

    const double spreadOff = pitchSpread (false);
    const double spreadOn  = pitchSpread (true);
    INFO ("spread off=" << spreadOff << " on=" << spreadOn);
    REQUIRE (spreadOn > spreadOff + 3.0);      // vibrato clearly widens the pitch spread
}

TEST_CASE ("sustain pedal holds notes after key release", "[plugin][midi][sustain]")
{
    Fixture f;
    // Note on, sustain down, note off (key), keep rendering -> should still sound;
    // then sustain up -> should release.
    { juce::AudioBuffer<float> b (2, 512); b.clear(); juce::MidiBuffer m;
      m.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
      m.addEvent (juce::MidiMessage::controllerEvent (1, 64, 127), 1);  // sustain down
      f.p.processBlock (b, m); }

    // release the KEY while pedal held
    auto held = f.run (30, juce::MidiMessage::noteOff (1, 60), 0);
    REQUIRE (rms (held, 512 * 20, 512) > 0.02);        // still sounding (pedal holds)

    // now release the pedal -> note should decay
    Fixture g;
    { juce::AudioBuffer<float> b (2, 512); b.clear(); juce::MidiBuffer m;
      m.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
      m.addEvent (juce::MidiMessage::controllerEvent (1, 64, 127), 1);
      m.addEvent (juce::MidiMessage::noteOff (1, 60), 2);
      g.p.processBlock (b, m); }
    auto rel = g.run (60, juce::MidiMessage::controllerEvent (1, 64, 0), 2);  // pedal up @ block 2
    REQUIRE (rms (rel, 512 * 50, 512) < 0.01);         // decayed after pedal release
}
