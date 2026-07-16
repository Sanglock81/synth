// ============================================================================
// Velocity connection (#54): the per-step velocity emitted by the sequencer/arp must
// actually reach the voice and shape the sound. In a synth, velocity drives the VCA
// (amplitude, via vel->amp) AND the filter (brightness, via vel->cutoff). This verifies
// the full chain end-to-end at the engine, across the real 0.1..2.0 scalar range that
// the 10..200 % UI produces — including the > 1.0 "accent" range (the old min(1.0) emit
// clamp made 100..200 % inert; this pins that it no longer does).
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "SynthEngine.h"
#include "test_util.h"
#include <vector>

namespace
{
    constexpr double kSR = 48000.0;

    // Render one sustained note at the given velocity; return a power-of-two steady-state
    // slice (well past the attack) suitable for both peak and FFT analysis.
    std::vector<float> renderNoteAtVel (VoiceParams p, float vel, int note = 57)
    {
        SynthEngine e;
        e.prepare (kSR);                       // default maxBlock; render in <= maxBlock chunks
        e.noteOn (note, vel);
        std::vector<float> out; out.reserve (12000);
        const int block = 512;
        std::vector<float> buf ((std::size_t) block);
        for (int done = 0; done < 12000; done += block)
        {
            std::fill (buf.begin(), buf.end(), 0.0f);
            e.render (buf.data(), block, p, 2.0f, 0, 0.0f, 0);
            out.insert (out.end(), buf.begin(), buf.end());
        }
        return tu::slice (out, 4096, 4096);    // 2^12 window in the sustain, for FFT + peak
    }

    // Spectral centroid (Hz) — a brightness proxy: higher = more high-frequency energy.
    double centroidHz (const std::vector<float>& x)
    {
        auto mag = tu::magnitudeSpectrum (x);
        const double binHz = kSR / double (x.size());   // magnitudeSpectrum keeps the FFT length
        double num = 0.0, den = 0.0;
        for (std::size_t k = 1; k < mag.size(); ++k) { num += double (k) * binHz * mag[k]; den += mag[k]; }
        return den > 0.0 ? num / den : 0.0;
    }

    VoiceParams sustainedSaw()
    {
        VoiceParams p;
        p.osc1Wave = 0; p.osc1Level = 0.8f; p.osc2Level = 0.0f; p.osc3Level = 0.0f;   // a bright saw (Wave::Saw)
        p.filterType = 0; p.cutoffHz = 700.0f; p.resonance = 0.1f;
        p.filterEnvAmt = 0.0f; p.keytrack = 0.0f;                                       // isolate velocity's effect
        p.ampA = 0.002f; p.ampD = 0.05f; p.ampS = 1.0f; p.ampR = 0.1f;                  // flat sustain to measure
        return p;
    }
}

TEST_CASE ("velocity drives amplitude (vel->amp) end-to-end through the engine", "[dsp][velocity]")
{
    VoiceParams p = sustainedSaw();
    p.velToAmp = 0.7f; p.velToCutoff = 0.0f;

    const float soft = tu::peak (renderNoteAtVel (p, 0.4f));
    const float loud = tu::peak (renderNoteAtVel (p, 1.0f));
    const float accent = tu::peak (renderNoteAtVel (p, 1.6f));   // > 1.0 accent (the formerly-dead range)

    REQUIRE (loud   > soft   * 1.2f);      // 100 % clearly louder than a 40 % ghost
    REQUIRE (accent > loud   * 1.1f);      // a 160 % accent is louder still — the upper range is LIVE
}

TEST_CASE ("velocity drives filter brightness (vel->cutoff) end-to-end through the engine", "[dsp][velocity]")
{
    VoiceParams p = sustainedSaw();
    p.velToAmp = 0.0f;                     // remove the level change so brightness is the ONLY variable
    p.velToCutoff = 0.6f;

    const double dark   = centroidHz (renderNoteAtVel (p, 0.4f));
    const double bright = centroidHz (renderNoteAtVel (p, 1.6f));   // harder hit -> more open filter

    REQUIRE (bright > dark * 1.15f);       // the accent is audibly brighter, not just louder
}

TEST_CASE ("with velocity routing off, velocity changes nothing (connection is via the routes)", "[dsp][velocity]")
{
    VoiceParams p = sustainedSaw();
    p.velToAmp = 0.0f; p.velToCutoff = 0.0f;   // both routes disabled

    const float a = tu::peak (renderNoteAtVel (p, 0.3f));
    const float b = tu::peak (renderNoteAtVel (p, 1.8f));
    REQUIRE (a == Catch::Approx (b).margin (0.02f));   // no route -> velocity is inert (as designed)
}
