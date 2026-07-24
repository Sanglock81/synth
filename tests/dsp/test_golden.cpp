// ============================================================================
// Golden-render regression. Renders a fixed 2 s sequence (C-major chord +
// exponential filter sweep) through the engine and compares against a committed
// reference WAV. Catches accidental sound changes from refactors.
//
// The sweep is driven at the VoiceParams level rather than through APVTS CC
// (this binary is JUCE-free and fully deterministic); the CC->param path is
// covered by the plugin-layer MIDI-learn tests.
//
// First run with no reference present writes it and passes; commit the file so
// later runs compare against it.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "SynthEngine.h"
#include "test_util.h"
#include <vector>
#include <string>
#include <cstdio>
#include <cmath>

namespace
{
    constexpr double kSR = 48000.0;

    std::vector<float> renderGolden()
    {
        SynthEngine engine;
        engine.prepare (kSR);

        VoiceParams p;
        p.osc1Wave = 0; p.osc2Wave = 0;            // saw/saw
        p.osc1Level = 0.5f; p.osc2Level = 0.5f;    // matches the legacy osc_mix=0.5 crossfade
        p.osc2Detune = 8.0f;                       // slight detune for motion
        p.filterType = 0;                          // LP
        p.resonance = 0.4f; p.filterEnvAmt = 0.0f; p.keytrack = 0.0f;
        p.ampA = 0.005f; p.ampD = 0.2f; p.ampS = 0.8f; p.ampR = 0.2f;
        // vel_to_amp default 0.9 with a PERCEPTUAL (dB-linear) vel->amp curve: at velocity 0.8 the
        // amp scale is ~0.44 (-7.2 dB). The golden was regenerated for this intended velocity change.

        const int total = int (kSR * 2.0);
        const int block = 256;
        std::vector<float> out (total, 0.0f);

        engine.noteOn (60, 0.8f);
        engine.noteOn (64, 0.8f);
        engine.noteOn (67, 0.8f);

        int cursor = 0;
        bool released = false;
        while (cursor < total)
        {
            const int n = std::min (block, total - cursor);
            const double frac = double (cursor) / double (total);
            p.cutoffHz = float (300.0 * std::pow (8000.0 / 300.0, frac));   // 300 -> 8k Hz

            if (! released && cursor >= int (kSR * 1.5))
            {
                engine.noteOff (60); engine.noteOff (64); engine.noteOff (67);
                released = true;
            }
            engine.render (out.data() + cursor, n, p, 2.0f, 0, 0.0f, 0);
            cursor += n;
        }
        return out;
    }
}

TEST_CASE ("golden render matches committed reference", "[golden][regression]")
{
    const std::string path = std::string (VASYNTH_GOLDEN_DIR) + "/render.f32.wav";
    auto rendered = renderGolden();

    REQUIRE (tu::allFinite (rendered));
    REQUIRE (tu::peak (rendered) > 0.05f);

    auto ref = tu::readWavF32 (path);
    if (ref.empty())
    {
        tu::writeWavF32 (path, rendered, int (kSR));
        WARN ("golden reference did not exist; wrote " << path << " — commit it and re-run");
        SUCCEED();
        return;
    }

    REQUIRE (ref.size() == rendered.size());

    double diffSq = 0.0, sigSq = 0.0; float maxAbs = 0.0f;
    for (std::size_t i = 0; i < ref.size(); ++i)
    {
        const double d = double (rendered[i]) - double (ref[i]);
        diffSq += d * d; sigSq += double (ref[i]) * double (ref[i]);
        maxAbs = std::max (maxAbs, std::abs (float (d)));
    }
    const double diffRms = std::sqrt (diffSq / double (ref.size()));
    const double sigRms  = std::sqrt (sigSq  / double (ref.size()));
    const double relDb   = tu::linToDb (diffRms / std::max (sigRms, 1e-12));

    INFO ("maxAbsDiff=" << maxAbs << " relDiff=" << relDb << " dB");
    REQUIRE (maxAbs < 5.0e-3f);      // small absolute tolerance
    REQUIRE (relDb  < -60.0);        // difference energy 60 dB below signal
}
