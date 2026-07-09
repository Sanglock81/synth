// ============================================================================
// Sub-phase 2 — per-part FX isolation + silent-part skip (engine level).
// Each part runs its OWN FX chain: a delay on one part must not colour another,
// and a part with no voices and idle FX must be skipped (the CPU control).
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "SynthEngine.h"
#include "test_util.h"
#include <vector>
#include <array>
#include <cmath>

namespace
{
    constexpr double kSR = 48000.0;

    VoiceParams pluck (int wave = 3)
    {
        VoiceParams p;
        p.osc1Wave = wave; p.osc2Wave = wave; p.osc1Level = 0.8f; p.osc2Level = 0.0f; p.osc3Level = 0.0f;
        p.cutoffHz = 16000.0f; p.resonance = 0.0f; p.filterEnvAmt = 0.0f; p.velToAmp = 0.0f;
        p.ampA = 0.002f; p.ampD = 0.03f; p.ampS = 0.0f; p.ampR = 0.02f;   // short: direct sound gone fast
        return p;
    }

    FXParams heavyDelay()
    {
        FXParams f;
        f.enabled[FXChain::Delay_] = true;
        f.delayTimeMs = 90.0f; f.delayFeedback = 0.6f; f.delayMix = 0.6f; f.delaySpread = 1.0f;
        return f;
    }

    // Render `blocks` 128-frame blocks, mono-summed (L), no new events.
    std::vector<float> run (SynthEngine& e, const std::array<FXParams, SynthEngine::maxParts>& fx, int blocks)
    {
        std::vector<float> out; std::vector<float> L (128), R (128);
        for (int b = 0; b < blocks; ++b)
        {
            e.renderMaster (L.data(), R.data(), 128, VoiceParams{}, 2.0f, 0, 0.0f, 0, fx.data());
            for (int i = 0; i < 128; ++i) out.push_back (L[i]);
        }
        return out;
    }

    // Energy in a window [fromSample, end).
    double tailEnergy (const std::vector<float>& x, int fromSample)
    {
        double e = 0.0;
        for (int i = fromSample; i < (int) x.size(); ++i) e += (double) x[(std::size_t) i] * x[(std::size_t) i];
        return e;
    }
}

TEST_CASE ("multitimbral: each part uses its OWN FX (delay isolation)", "[multi][fx]")
{
    const int tailFrom = 60 * 128;   // well after the (short) direct note

    // (a) a DRY part leaves no tail.
    {
        SynthEngine e; e.setMaxVoices (16); e.prepare (kSR, 128);
        std::array<FXParams, SynthEngine::maxParts> fx {};        // all dry
        e.setLockedPartParams (1, pluck());
        e.noteOn (60, 1.0f, 1); auto out = run (e, fx, 200);
        REQUIRE (tailEnergy (out, tailFrom) < 1.0e-4);
    }
    // (b) the SAME part with a delay produces a tail of repeats.
    {
        SynthEngine e; e.setMaxVoices (16); e.prepare (kSR, 128);
        std::array<FXParams, SynthEngine::maxParts> fx {};
        fx[1] = heavyDelay();
        e.setLockedPartParams (1, pluck());
        e.noteOn (60, 1.0f, 1); auto out = run (e, fx, 200);
        REQUIRE (tailEnergy (out, tailFrom) > 1.0e-3);           // delayed repeats present
    }
    // (c) isolation: part 2 has the delay, but we play part 1 (dry) -> NO tail.
    {
        SynthEngine e; e.setMaxVoices (16); e.prepare (kSR, 128);
        std::array<FXParams, SynthEngine::maxParts> fx {};
        fx[2] = heavyDelay();                                    // delay on the OTHER part
        e.setLockedPartParams (1, pluck());
        e.noteOn (60, 1.0f, 1); auto out = run (e, fx, 200);     // play part 1
        REQUIRE (tailEnergy (out, tailFrom) < 1.0e-4);           // part 2's delay never touches part 1
    }
}

TEST_CASE ("multitimbral: silent parts with idle FX are skipped", "[multi][skip]")
{
    SynthEngine e; e.setMaxVoices (16); e.prepare (kSR, 128);
    std::array<FXParams, SynthEngine::maxParts> fx {};           // all dry
    std::vector<float> L (128), R (128);

    // Only part 0 sounding -> exactly one part processed.
    e.setLockedPartParams (1, pluck());
    e.noteOn (60, 0.8f, 0);
    e.renderMaster (L.data(), R.data(), 128, pluck(), 2.0f, 0, 0.0f, 0, fx.data());
    REQUIRE (e.partsProcessedLastBlock() == 1);

    // Add a voice on part 1 -> two parts processed.
    e.noteOn (48, 0.8f, 1);
    e.renderMaster (L.data(), R.data(), 128, pluck(), 2.0f, 0, 0.0f, 0, fx.data());
    REQUIRE (e.partsProcessedLastBlock() == 2);

    // Nothing sounding at all -> zero parts processed (full skip).
    e.allNotesOff();
    for (int b = 0; b < 40; ++b) e.renderMaster (L.data(), R.data(), 128, pluck(), 2.0f, 0, 0.0f, 0, fx.data());
    REQUIRE (e.partsProcessedLastBlock() == 0);
}

TEST_CASE ("multitimbral: a part's delay TAIL keeps processing after its voice ends", "[multi][skip]")
{
    SynthEngine e; e.setMaxVoices (16); e.prepare (kSR, 128);
    std::array<FXParams, SynthEngine::maxParts> fx {};
    fx[1] = heavyDelay();
    std::vector<float> L (128), R (128);

    e.setLockedPartParams (1, pluck());
    e.noteOn (60, 1.0f, 1);
    e.renderMaster (L.data(), R.data(), 128, VoiceParams{}, 2.0f, 0, 0.0f, 0, fx.data());
    e.noteOff (60, 1);
    // A few blocks after the note: part 1 has no voice but its delay is still ringing,
    // so it must NOT be skipped yet.
    for (int b = 0; b < 8; ++b) e.renderMaster (L.data(), R.data(), 128, VoiceParams{}, 2.0f, 0, 0.0f, 0, fx.data());
    REQUIRE (e.partsProcessedLastBlock() >= 1);
}
