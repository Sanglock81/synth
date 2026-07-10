// ============================================================================
// Sub-phase 2 — per-part FX isolation + silent-part skip (engine level).
// Each part runs its OWN FX chain: a delay on one part must not colour another,
// and a part with no voices and idle FX must be skipped (the CPU control).
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "SynthEngine.h"
#include "test_util.h"
#include <vector>
#include <array>
#include <cmath>
#include <cstdio>

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

TEST_CASE ("multitimbral: each part has its OWN LFOs (independent modulation)", "[multi][lfo]")
{
    // A resonant low-pass; a cutoff LFO makes the output wobble in level/brightness.
    auto resoVoice = [] { VoiceParams p; p.osc1Wave = 0; p.osc1Level = 0.8f; p.osc2Level = 0.0f;
                          p.cutoffHz = 700.0f; p.resonance = 0.7f; p.filterEnvAmt = 0.0f; p.velToAmp = 0.0f;
                          p.ampA = 0.002f; p.ampD = 0.02f; p.ampS = 0.9f; p.ampR = 0.05f; return p; };
    // Coefficient of variation of per-window RMS — high => the signal is being modulated.
    auto wobble = [] (const std::vector<float>& x)
    {
        std::vector<double> r; const int w = 1024;
        for (int s = 0; s + w <= (int) x.size(); s += w)
        { double a = 0; for (int i = 0; i < w; ++i) a += (double) x[(std::size_t) (s + i)] * x[(std::size_t) (s + i)];
          r.push_back (std::sqrt (a / w)); }
        double mean = 0; for (double v : r) mean += v; mean /= (double) r.size();
        double var = 0; for (double v : r) var += (v - mean) * (v - mean); var /= (double) r.size();
        return mean > 1e-6 ? std::sqrt (var) / mean : 0.0;
    };

    const int blocks = 300;
    std::array<FXParams, SynthEngine::maxParts> fx {};
    PartLfos cutoffLfo {}; cutoffLfo.lfo[0] = { 6.0f, 1.0f, 1 /*sine*/, 2 /*cutoff*/ };

    // (a) part 0 with a cutoff LFO -> strong wobble.
    double wOn, wOff, wIso;
    {
        SynthEngine e; e.setMaxVoices (16); e.prepare (kSR, 128);
        std::array<PartLfos, SynthEngine::maxParts> pl {}; pl[0] = cutoffLfo;
        std::vector<float> out, L (128), R (128);
        e.noteOn (60, 0.9f, 0);
        for (int b = 0; b < blocks; ++b) { e.renderMaster (L.data(), R.data(), 128, resoVoice(), pl.data(), fx.data());
                                           for (int i = 0; i < 128; ++i) out.push_back (L[i]); }
        wOn = wobble (out);
    }
    // (b) part 0 with NO LFO -> steady.
    {
        SynthEngine e; e.setMaxVoices (16); e.prepare (kSR, 128);
        std::array<PartLfos, SynthEngine::maxParts> pl {};
        std::vector<float> out, L (128), R (128);
        e.noteOn (60, 0.9f, 0);
        for (int b = 0; b < blocks; ++b) { e.renderMaster (L.data(), R.data(), 128, resoVoice(), pl.data(), fx.data());
                                           for (int i = 0; i < 128; ++i) out.push_back (L[i]); }
        wOff = wobble (out);
    }
    // (c) LFO configured on part 1, but we play part 0 -> steady (isolation).
    {
        SynthEngine e; e.setMaxVoices (16); e.prepare (kSR, 128);
        std::array<PartLfos, SynthEngine::maxParts> pl {}; pl[1] = cutoffLfo;   // on the OTHER part
        std::vector<float> out, L (128), R (128);
        e.noteOn (60, 0.9f, 0);                                                // play part 0
        for (int b = 0; b < blocks; ++b) { e.renderMaster (L.data(), R.data(), 128, resoVoice(), pl.data(), fx.data());
                                           for (int i = 0; i < 128; ++i) out.push_back (L[i]); }
        wIso = wobble (out);
    }
    INFO ("wOn=" << wOn << " wOff=" << wOff << " wIso=" << wIso);
    REQUIRE (wOn > 0.10);              // part 0's own LFO modulates it
    REQUIRE (wOff < wOn * 0.25);       // no LFO -> steady
    REQUIRE (wIso < wOn * 0.25);       // part 1's LFO never touches part 0
}

TEST_CASE ("mixer: level scales and pan positions with a 0 dB-centre law", "[multi][mix]")
{
    auto sus = [] { VoiceParams p; p.osc1Wave = 3; p.osc1Level = 0.8f; p.osc2Level = 0.0f;
                    p.cutoffHz = 16000.0f; p.resonance = 0.0f; p.filterEnvAmt = 0.0f; p.velToAmp = 0.0f;
                    p.ampA = 0.002f; p.ampD = 0.02f; p.ampS = 0.9f; p.ampR = 0.05f; return p; };

    auto render = [&] (float level, float pan)
    {
        SynthEngine e; e.setMaxVoices (16); e.prepare (kSR, 128);
        FXParams fx0 {}; PartLfos lfo0 {};
        std::array<float, SynthEngine::maxParts> lv { { 1, 1, 1, 1 } }, pn {};
        lv[0] = level; pn[0] = pan;
        e.noteOn (60, 0.9f, 0);
        std::vector<float> L, R, l (128), r (128);
        for (int b = 0; b < 80; ++b)
        {
            e.beginMasterBlock (128, sus(), fx0, lfo0);
            e.setMix (lv, pn);
            e.renderParts (0, 128, sus());
            e.mixParts (l.data(), r.data(), 128);
            for (int i = 0; i < 128; ++i) { L.push_back (l[i]); R.push_back (r[i]); }
        }
        return std::pair<double, double> { tu::rms (L), tu::rms (R) };
    };

    const auto [lFull,  rFull]  = render (1.0f,  0.0f);   // unity, centre
    const auto [lHalf,  rHalf]  = render (0.5f,  0.0f);   // half level
    const auto [lLeft,  rLeft]  = render (1.0f, -1.0f);   // hard left
    const auto [lRight, rRight] = render (1.0f,  1.0f);   // hard right

    REQUIRE (lFull > 0.01);
    REQUIRE (lFull == Catch::Approx (rFull));             // centre: L == R
    REQUIRE (lHalf == Catch::Approx (lFull * 0.5).epsilon (0.02));   // level scales linearly
    REQUIRE (lLeft == Catch::Approx (lFull).epsilon (0.02));         // 0 dB centre: left unchanged at hard-left
    REQUIRE (rLeft < lLeft * 0.01);                       // ...and right silent
    REQUIRE (rRight == Catch::Approx (rFull).epsilon (0.02));
    REQUIRE (lRight < rRight * 0.01);
}

TEST_CASE ("mixer: sweeping level/pan per block does not zipper (smoothed)", "[multi][mix][zipper]")
{
    auto sus = [] { VoiceParams p; p.osc1Wave = 3; p.osc1Level = 0.8f; p.osc2Level = 0.0f;
                    p.cutoffHz = 16000.0f; p.resonance = 0.0f; p.filterEnvAmt = 0.0f; p.velToAmp = 0.0f;
                    p.ampA = 0.002f; p.ampD = 0.02f; p.ampS = 0.9f; p.ampR = 0.05f; return p; };

    SynthEngine e; e.setMaxVoices (16); e.prepare (kSR, 128);
    FXParams fx0 {}; PartLfos lfo0 {};
    e.noteOn (60, 0.9f, 0);

    std::vector<float> L (128), R (128);
    float prev = 0.0f, maxJump = 0.0f;
    for (int b = 0; b < 400; ++b)
    {
        // Abrupt per-block level + pan sweep — a raw (unsmoothed) mixer would step-zipper.
        std::array<float, SynthEngine::maxParts> lv { { 1, 1, 1, 1 } }, pn {};
        lv[0] = 0.4f + 1.4f * (float) (b % 20) / 20.0f;                 // sweep 0.4..1.8
        pn[0] = -0.9f + 1.8f * (float) ((b * 3) % 20) / 20.0f;          // pan sweep
        e.beginMasterBlock (128, sus(), fx0, lfo0);
        e.setMix (lv, pn);
        e.renderParts (0, 128, sus());
        e.mixParts (L.data(), R.data(), 128);
        for (int i = 0; i < 128; ++i) { maxJump = std::max (maxJump, std::abs (L[i] - prev)); prev = L[i]; }
    }
    INFO ("maxJump=" << maxJump);
    REQUIRE (maxJump < 0.05f);   // smoothed gains -> no zipper across abrupt level/pan steps
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

TEST_CASE ("multitimbral: FX skip -> retrigger resumes without a click (stale-state)", "[multi][skip][click]")
{
    // Reproduces the static/pops regression: a part with FX plays, goes silent long
    // enough that its FX processing is SKIPPED, then retriggers. If the skipped chain
    // kept stale delay/reverb/chorus state, the resume boundary pops.
    auto susSaw = [] { VoiceParams p; p.osc1Wave = 0; p.osc1Level = 0.7f; p.osc2Level = 0.0f;
                       p.cutoffHz = 12000.0f; p.resonance = 0.1f; p.filterEnvAmt = 0.0f; p.velToAmp = 0.0f;
                       p.ampA = 0.005f; p.ampD = 0.02f; p.ampS = 0.8f; p.ampR = 0.03f; return p; };

    for (int fxKind = 0; fxKind < FXChain::kNumFX; ++fxKind)
    {
        SynthEngine e; e.setMaxVoices (16); e.prepare (kSR, 128);
        std::array<FXParams, SynthEngine::maxParts> fx {}; PartLfos lfo0 {};
        fx[0].enabled[fxKind] = true;
        fx[0].delayTimeMs = 80.0f; fx[0].delayFeedback = 0.45f; fx[0].delayMix = 0.6f;
        fx[0].reverbMix = 0.6f; fx[0].reverbSize = 0.6f; fx[0].chorusMix = 0.6f; fx[0].width = 1.6f;
        std::array<PartLfos, SynthEngine::maxParts> lfo {};

        std::vector<float> L (128), R (128);
        auto block = [&] { e.renderMaster (L.data(), R.data(), 128, susSaw(), lfo.data(), fx.data()); };

        e.noteOn (60, 0.9f, 0); block();
        e.noteOff (60, 0);
        // Render until the FX has gone silent and the part is being SKIPPED.
        int skipBlocks = 0;
        for (int b = 0; b < 4000 && skipBlocks < 8; ++b) { block(); if (e.partsProcessedLastBlock() == 0) ++skipBlocks; else skipBlocks = 0; }
        REQUIRE (skipBlocks >= 8);                          // skip really engaged

        // Retrigger and scan the resume boundary for a click / non-finite.
        e.noteOn (60, 0.9f, 0);
        std::vector<float> resume;
        for (int b = 0; b < 20; ++b) { block(); for (int i = 0; i < 128; ++i) { resume.push_back (L[i]); resume.push_back (R[i]); } }

        REQUIRE (tu::allFinite (resume));
        REQUIRE (tu::peak (resume) <= 1.0f);
        float maxJump = 0.0f;
        for (std::size_t i = 2; i < resume.size(); ++i) maxJump = std::max (maxJump, std::abs (resume[i] - resume[i - 2]));  // per-channel delta
        INFO ("fxKind=" << fxKind << " maxJump=" << maxJump);
        REQUIRE (maxJump < 0.20f);
    }
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
