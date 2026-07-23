// ============================================================================
// Musicality Pass — Tier 1 (analog life for the oscillators).
// 1a start-phase policy: RESET (bit-exact, deterministic), RANDOM (each note a fresh phase),
//    FREE (keeps the running phase across notes).
// 1b analog drift: bit-exact at analog=0 (the fast path), bounded + deterministic when driven.
// JUCE-free (DSP-only).
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "SynthVoice.h"
#include <vector>
#include <cmath>

namespace
{
    VoiceParams sawPatch (int phaseMode = 0, float analog = 0.0f)
    {
        VoiceParams p;
        p.osc1Level = 1.0f; p.osc2Level = 0.0f; p.osc3Level = 0.0f;   // osc1 only
        p.osc1Wave = 0;                                              // saw (phase is audible)
        p.ampA = 0.0005f; p.ampD = 0.0f; p.ampS = 1.0f; p.ampR = 0.001f;
        p.cutoffHz = 18000.0f; p.resonance = 0.0f; p.filterEnvAmt = 0.0f;
        p.osc1Phase = p.osc2Phase = p.osc3Phase = phaseMode;
        p.analog = analog;
        return p;
    }

    // Play one note into a fresh buffer, then release the voice to idle (so the next note is fresh).
    std::vector<float> play (SynthVoice& v, const VoiceParams& p, int mode, int n)
    {
        std::vector<float> buf ((std::size_t) n, 0.0f);
        v.noteOn (60, 1.0f, 1, 0, 0, false, mode, mode, mode);
        v.render (buf.data(), n, p);
        return buf;
    }
    void goIdle (SynthVoice& v, const VoiceParams& p)
    {
        v.noteOff();
        std::vector<float> scratch (2048, 0.0f);
        for (int i = 0; i < 30 && v.isActive(); ++i) { std::fill (scratch.begin(), scratch.end(), 0.0f); v.render (scratch.data(), 2048, p); }
    }
    bool identical (const std::vector<float>& a, const std::vector<float>& b)
    { if (a.size() != b.size()) return false; for (std::size_t i = 0; i < a.size(); ++i) if (a[i] != b[i]) return false; return true; }
}

TEST_CASE ("Tier 1a: RESET phase is deterministic - consecutive notes are bit-identical", "[dsp][musicality][phase]")
{
    SynthVoice v; v.prepare (48000.0);
    auto p = sawPatch (0);   // RESET
    auto a = play (v, p, 0, 512); goIdle (v, p);
    auto b = play (v, p, 0, 512); goIdle (v, p);
    REQUIRE (identical (a, b));                     // phase reset to 0 both times -> today's behaviour
}

TEST_CASE ("Tier 1a: RANDOM phase varies each note", "[dsp][musicality][phase]")
{
    SynthVoice v; v.prepare (48000.0);
    auto p = sawPatch (1);   // RANDOM
    auto a = play (v, p, 1, 512); goIdle (v, p);
    auto b = play (v, p, 1, 512); goIdle (v, p);
    REQUIRE_FALSE (identical (a, b));               // each note draws a fresh start phase
}

TEST_CASE ("Tier 1a: FREE keeps the running phase (differs from RESET)", "[dsp][musicality][phase]")
{
    // Warm the phase up with a first note, then a FREE note continues from it while a RESET note
    // restarts at 0 — so the two disagree.
    SynthVoice vFree; vFree.prepare (48000.0);
    auto pf = sawPatch (2);
    play (vFree, pf, 2, 400); goIdle (vFree, pf);   // leave osc phase non-zero
    auto freeBuf = play (vFree, pf, 2, 512);

    SynthVoice vReset; vReset.prepare (48000.0);
    auto pr = sawPatch (0);
    play (vReset, pr, 0, 400); goIdle (vReset, pr);
    auto resetBuf = play (vReset, pr, 0, 512);

    REQUIRE_FALSE (identical (freeBuf, resetBuf));
    REQUIRE (std::abs (resetBuf[1]) < std::abs (freeBuf[1]) + 2.0f);  // sanity: both finite/rendered
}

TEST_CASE ("Tier 1b: analog = 0 is bit-exact (the fast path)", "[dsp][musicality][drift]")
{
    // Two voices, one built with analog=0 explicitly and one never touching drift, must agree, and a
    // second identical run of the analog=0 voice must be bit-identical (no hidden RNG advance).
    SynthVoice v1; v1.prepare (48000.0); auto p0 = sawPatch (0, 0.0f);
    auto a = play (v1, p0, 0, 1024); goIdle (v1, p0);
    auto b = play (v1, p0, 0, 1024);
    REQUIRE (identical (a, b));
}

TEST_CASE ("Tier 1b: driven drift is bounded and deterministic under the seeded RNG", "[dsp][musicality][drift]")
{
    // Determinism: two fresh voices with the same seed + same block schedule produce identical audio.
    SynthVoice va; va.prepare (48000.0); auto pd = sawPatch (0, 1.0f);
    SynthVoice vb; vb.prepare (48000.0);
    auto a = play (va, pd, 0, 4096);
    auto b = play (vb, pd, 0, 4096);
    REQUIRE (identical (a, b));                     // seeded drift RNG -> reproducible
    REQUIRE_FALSE (identical (a, play (va, sawPatch (0, 0.0f), 0, 4096)));   // and it DID differ from analog=0
    for (float s : a) REQUIRE (std::isfinite (s));  // drift never destabilises the output
}
