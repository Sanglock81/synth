// Synth — Copyright © 2026 John L Farmer. Licensed under AGPLv3; see LICENSE.
// ============================================================================
// DSP performance benchmark (JUCE-free). Measures worst-case 128-sample block
// render time at 48 kHz for the full engine, across oscillator quality modes,
// so we can keep VA Synth glitch-free on modest hardware (2015-class dual-core,
// the live machine).
//
// Not a CTest gate (wall time is machine-dependent). Run before/after DSP
// changes to watch the budget:  ./build/tests/dsp_bench
//
// Real-time budget for a 128-sample block at 48 kHz = 128/48000 = 2.667 ms.
// The synth must render one block in well under that, with headroom.
// ============================================================================
#include "SynthEngine.h"
#include "FXChain.h"
#include <chrono>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <string>

namespace
{
    using clk = std::chrono::steady_clock;
    constexpr double kSR       = 48000.0;
    constexpr int    kBlock    = 128;
    constexpr double kBudgetMs = kBlock / kSR * 1000.0;     // 2.667 ms
    // No dev->target derate: this bench runs ON the reference target (Intel i7-8650U ThinkPad),
    // so the measured p99 at the PERFORMANCE governor IS the target figure. The old assumed x3.5
    // (from wrongly believing the target was a separate, slower "2015 dual-core" machine) inflated
    // target-native numbers ~3.5x; corrected to x1.0. Governor still matters — measure at performance.
    constexpr double kTargetDerate = 1.0;

    struct Stat { double medMs, p99Ms, maxMs; };

    // Render `blocks` blocks of `voices` held notes; return median / p99 / max
    // block ms. p99 is the robust "worst-case" (max on a non-realtime dev box is
    // dominated by OS preemption outliers, not DSP cost).
    Stat measure (PolyBlepOscillator::Quality q, int voices, int blocks, int oscsOn = 2, float drive = 0.0f)
    {
        SynthEngine engine;
        engine.setOscQuality (q);
        engine.setMaxVoices (voices);
        engine.prepare (kSR);

        VoiceParams p;                       // saws, worst case (oversampled)
        p.osc1Wave = p.osc2Wave = p.osc3Wave = 0;
        p.osc1Level = 0.8f;
        p.osc2Level = oscsOn >= 2 ? 0.8f : 0.0f;
        p.osc3Level = oscsOn >= 3 ? 0.8f : 0.0f;
        p.cutoffHz = 2000.0f; p.resonance = 0.4f; p.filterEnvAmt = 0.5f;
        p.drive = drive;                     // 2C: >0 puts every voice's filter on the 2x oversampled path
        for (int i = 0; i < voices; ++i) engine.noteOn (36 + i, 0.7f);

        std::vector<float> out (kBlock, 0.0f);
        for (int i = 0; i < 64; ++i)         // warm caches / branch predictors
            engine.render (out.data(), kBlock, p, 3.0f, 0, 0.3f, 2);

        std::vector<double> times (blocks);
        for (int b = 0; b < blocks; ++b)
        {
            const auto t0 = clk::now();
            engine.render (out.data(), kBlock, p, 3.0f, 0, 0.3f, 2);
            const auto t1 = clk::now();
            times[b] = std::chrono::duration<double, std::milli> (t1 - t0).count();
        }
        std::sort (times.begin(), times.end());
        return { times[blocks / 2], times[(int) (blocks * 0.99)], times.back() };
    }

    // Full per-block path: engine render (mono) + duplicate to stereo + FX chain,
    // exactly as the processor runs it. `fxMask` bit 0..3 = chorus/delay/reverb/width.
    Stat measureFull (int voices, int blocks, int oscsOn, int fxMask, float reverbMotion = 0.0f, int chorusVoices = 1, float sat = 0.0f)
    {
        SynthEngine engine;
        engine.setOscQuality (PolyBlepOscillator::Quality::Efficient);
        engine.setMaxVoices (voices);
        engine.prepare (kSR);

        VoiceParams p;
        p.osc1Wave = p.osc2Wave = p.osc3Wave = 0;
        p.osc1Level = 0.8f;
        p.osc2Level = oscsOn >= 2 ? 0.8f : 0.0f;
        p.osc3Level = oscsOn >= 3 ? 0.8f : 0.0f;
        p.cutoffHz = 2000.0f; p.resonance = 0.4f; p.filterEnvAmt = 0.5f;
        for (int i = 0; i < voices; ++i) engine.noteOn (36 + i, 0.7f);

        FXChain fx; fx.prepare (kSR, kBlock);
        FXParams fp;
        fp.enabled[FXChain::Chorus_] = (fxMask & 1) != 0;
        fp.enabled[FXChain::Delay_]  = (fxMask & 2) != 0;
        fp.enabled[FXChain::Reverb_] = (fxMask & 4) != 0;
        fp.enabled[FXChain::Width_]  = (fxMask & 8) != 0;
        fp.chorusMix = 0.5f; fp.delayMix = 0.4f; fp.reverbMix = 0.4f; fp.width = 1.5f;
        fp.reverbMotion = reverbMotion;
        fp.chorusVoices = chorusVoices;
        fp.sat = sat;
        fx.setParams (fp);

        std::vector<float> mono (kBlock, 0.0f), L (kBlock, 0.0f), R (kBlock, 0.0f);
        auto oneBlock = [&]
        {
            engine.render (mono.data(), kBlock, p, 3.0f, 0, 0.3f, 2);
            std::copy (mono.begin(), mono.end(), L.begin());
            std::copy (mono.begin(), mono.end(), R.begin());
            fx.process (L.data(), R.data(), kBlock);
        };
        for (int i = 0; i < 200; ++i) oneBlock();     // warm + settle any crossfade

        std::vector<double> times (blocks);
        for (int b = 0; b < blocks; ++b)
        {
            const auto t0 = clk::now();
            oneBlock();
            const auto t1 = clk::now();
            times[b] = std::chrono::duration<double, std::milli> (t1 - t0).count();
        }
        std::sort (times.begin(), times.end());
        return { times[blocks / 2], times[(int) (blocks * 0.99)], times.back() };
    }

    // Kit worst case (Sub-phase 1): a live chord on part 0 + a Kit part filling the pool
    // with sustained pads, so every voice renders and 12 of them go through the Kit
    // paramsFor branch. Full stereo + FX path, like the processor.
    Stat measureKit (int blocks, int fxMask)
    {
        SynthEngine engine;
        engine.setOscQuality (PolyBlepOscillator::Quality::Efficient);
        engine.setMaxVoices (16);
        engine.prepare (kSR);

        VoiceParams live;
        live.osc1Wave = live.osc2Wave = 0; live.osc1Level = 0.8f; live.osc2Level = 0.8f;
        live.cutoffHz = 2000.0f; live.resonance = 0.4f; live.filterEnvAmt = 0.5f;
        for (int i = 0; i < 4; ++i) engine.noteOn (60 + i, 0.7f, 0);          // live chord (part 0)

        KitData kit; kit.isKit = true;
        for (int i = 0; i < 12; ++i) { kit.pads[(std::size_t) i] = { 40 + i, { 40 + i, 0, 0, 0 }, 1, 0 };
                                       kit.params[(std::size_t) i] = live; }
        engine.setPartKit (1, kit);
        for (int i = 0; i < 12; ++i) engine.kitNoteOn (1, 40 + i, 0.7f);      // 12 sustained pads -> 16 voices

        FXChain fx; fx.prepare (kSR, kBlock);
        FXParams fp;
        fp.enabled[FXChain::Chorus_] = (fxMask & 1) != 0; fp.enabled[FXChain::Delay_] = (fxMask & 2) != 0;
        fp.enabled[FXChain::Reverb_] = (fxMask & 4) != 0; fp.enabled[FXChain::Width_] = (fxMask & 8) != 0;
        fp.chorusMix = 0.5f; fp.delayMix = 0.4f; fp.reverbMix = 0.4f; fp.width = 1.5f;
        fx.setParams (fp);

        std::vector<float> mono (kBlock, 0.0f), L (kBlock, 0.0f), R (kBlock, 0.0f);
        auto oneBlock = [&] { engine.render (mono.data(), kBlock, live, 3.0f, 0, 0.3f, 2);
                              std::copy (mono.begin(), mono.end(), L.begin()); std::copy (mono.begin(), mono.end(), R.begin());
                              fx.process (L.data(), R.data(), kBlock); };
        for (int i = 0; i < 200; ++i) oneBlock();
        std::vector<double> times (blocks);
        for (int b = 0; b < blocks; ++b)
        { const auto t0 = clk::now(); oneBlock(); const auto t1 = clk::now();
          times[(std::size_t) b] = std::chrono::duration<double, std::milli> (t1 - t0).count(); }
        std::sort (times.begin(), times.end());
        return { times[(std::size_t) (blocks / 2)], times[(std::size_t) (int) (blocks * 0.99)], times.back() };
    }

    // Sub-phase 2 worst case: `activeParts` parts all sounding at once, each running its
    // OWN FX chain (all four effects), 16 voices spread across them, Efficient. This is
    // the full-multitimbral gate. fxParts = how many parts have FX engaged.
    Stat measureMulti (int activeParts, int voicesPerPart, int fxParts, int blocks)
    {
        SynthEngine engine;
        engine.setOscQuality (PolyBlepOscillator::Quality::Efficient);
        engine.setMaxVoices (16);
        engine.prepare (kSR, kBlock);

        VoiceParams vp;
        vp.osc1Wave = vp.osc2Wave = vp.osc3Wave = 0;
        vp.osc1Level = 0.8f; vp.osc2Level = 0.8f; vp.osc3Level = 0.8f;
        vp.cutoffHz = 2000.0f; vp.resonance = 0.4f; vp.filterEnvAmt = 0.5f; vp.ampS = 0.9f;

        std::array<FXParams, SynthEngine::maxParts> fx {};
        std::array<PartLfos, SynthEngine::maxParts> lfo {};
        for (int p = 0; p < activeParts && p < SynthEngine::maxParts; ++p)
        {
            if (p >= 1) engine.setLockedPartParams (p, vp);
            if (p < fxParts)
                for (int f = 0; f < FXChain::kNumFX; ++f) fx[(std::size_t) p].enabled[f] = true;
            lfo[(std::size_t) p].lfo[0] = { 3.0f, 0.5f, 0, 2 };   // a cutoff LFO on every active part
            for (int v = 0; v < voicesPerPart; ++v) engine.noteOn (36 + p * 12 + v, 0.7f, p);
        }

        std::vector<float> L (kBlock, 0.0f), R (kBlock, 0.0f);
        for (int i = 0; i < 200; ++i) engine.renderMaster (L.data(), R.data(), kBlock, vp, lfo.data(), fx.data());

        std::vector<double> times (blocks);
        for (int b = 0; b < blocks; ++b)
        {
            const auto t0 = clk::now();
            engine.renderMaster (L.data(), R.data(), kBlock, vp, lfo.data(), fx.data());
            const auto t1 = clk::now();
            times[(std::size_t) b] = std::chrono::duration<double, std::milli> (t1 - t0).count();
        }
        std::sort (times.begin(), times.end());
        return { times[(std::size_t) (blocks / 2)], times[(std::size_t) (int) (blocks * 0.99)], times.back() };
    }

    // A single LOCKED part configured with arbitrary VoiceParams, so #96 UNISON, #132 FM, filter
    // DRIVE and self-oscillating RESONANCE all LATCH at note-on (they're read from paramsFor(part)
    // at noteOn, which the part-0 liveParams path can't set before the note). Full renderMaster
    // path: per-part FX (all five when allFx) + a cutoff LFO. `voices` = held notes = pool size.
    Stat measureLocked (PolyBlepOscillator::Quality q, VoiceParams vp, int voices, bool allFx, int blocks)
    {
        SynthEngine engine;
        engine.setOscQuality (q);
        engine.setMaxVoices (voices);
        engine.prepare (kSR, kBlock);
        engine.setLockedPartParams (1, vp);

        std::array<FXParams, SynthEngine::maxParts> fx {};
        std::array<PartLfos, SynthEngine::maxParts> lfo {};
        if (allFx)
        {
            for (int f = 0; f < FXChain::kNumFX; ++f) fx[1].enabled[(std::size_t) f] = true;
            fx[1].chorusMix = 0.5f; fx[1].delayMix = 0.4f; fx[1].reverbMix = 0.4f; fx[1].width = 1.5f; fx[1].reverbMotion = 1.0f;
        }
        lfo[1].lfo[0] = { 3.0f, 0.5f, 0, 2 };                          // a synced-style cutoff LFO
        for (int v = 0; v < voices; ++v) engine.noteOn (36 + v, 0.7f, 1);

        VoiceParams live {};                                          // part 0 idle
        std::vector<float> L (kBlock, 0.0f), R (kBlock, 0.0f);
        for (int i = 0; i < 200; ++i) engine.renderMaster (L.data(), R.data(), kBlock, live, lfo.data(), fx.data());
        std::vector<double> times ((std::size_t) blocks);
        for (int b = 0; b < blocks; ++b)
        {
            const auto t0 = clk::now();
            engine.renderMaster (L.data(), R.data(), kBlock, live, lfo.data(), fx.data());
            const auto t1 = clk::now();
            times[(std::size_t) b] = std::chrono::duration<double, std::milli> (t1 - t0).count();
        }
        std::sort (times.begin(), times.end());
        return { times[(std::size_t) (blocks / 2)], times[(std::size_t) (int) (blocks * 0.99)], times.back() };
    }

    // The realistic LIVE-SET scene the zero-xrun ship rule keys on: a lead (part 0) + a lush
    // UNISON pad (part 1) + an FM+driven bass (part 2) + a Classic-Machines-style KIT (part 3, 12
    // sustained pads = the sequenced drums), EVERY part with its own full FX chain + a cutoff LFO,
    // and note density standing in for a looper replaying content. Not a synthetic worst case —
    // the "does a real gig glitch?" test.
    Stat measureLiveSet (int blocks)
    {
        SynthEngine engine;
        engine.setOscQuality (PolyBlepOscillator::Quality::Efficient);
        engine.setMaxVoices (24);
        engine.prepare (kSR, kBlock);

        VoiceParams pad;                                              // part 1: lush unison pad
        pad.osc1Wave = pad.osc2Wave = pad.osc3Wave = 0;
        pad.osc1Level = pad.osc2Level = pad.osc3Level = 0.8f;
        pad.cutoffHz = 2000.0f; pad.resonance = 0.5f; pad.filterEnvAmt = 0.5f; pad.ampS = 0.9f;
        pad.unisonCount = 3; pad.unisonDetune = 0.2f; pad.unisonWidth = 0.6f;
        engine.setLockedPartParams (1, pad);

        VoiceParams bass;                                            // part 2: FM + driven bass
        bass.osc1Wave = bass.osc2Wave = 3; bass.osc1Level = 0.9f; bass.osc2Level = 0.6f;
        bass.osc1Fm = 0.6f; bass.cutoffHz = 1200.0f; bass.resonance = 0.6f; bass.drive = 0.6f; bass.ampS = 0.9f;
        engine.setLockedPartParams (2, bass);

        KitData kit; kit.isKit = true;                               // part 3: sequenced drum kit
        VoiceParams drum; drum.osc1Wave = 0; drum.osc1Level = 0.8f; drum.cutoffHz = 6000.0f; drum.ampS = 0.0f; drum.ampD = 0.08f;
        for (int i = 0; i < 12; ++i) { kit.pads[(std::size_t) i] = { 40 + i, { 40 + i, 0, 0, 0 }, 1, 0 };
                                       kit.params[(std::size_t) i] = drum; }
        engine.setPartKit (3, kit);

        std::array<FXParams, SynthEngine::maxParts> fx {};
        std::array<PartLfos, SynthEngine::maxParts> lfo {};
        for (int p = 1; p <= 3; ++p)
        {
            for (int f = 0; f < FXChain::kNumFX; ++f) fx[(std::size_t) p].enabled[(std::size_t) f] = true;
            fx[(std::size_t) p].chorusMix = 0.4f; fx[(std::size_t) p].delayMix = 0.35f;
            fx[(std::size_t) p].reverbMix = 0.4f; fx[(std::size_t) p].width = 1.4f; fx[(std::size_t) p].reverbMotion = 1.0f;
        }
        lfo[1].lfo[0] = { 3.0f, 0.5f, 0, 2 };
        lfo[2].lfo[0] = { 2.0f, 0.4f, 0, 2 };

        VoiceParams lead;                                            // part 0 (live): lead triad
        lead.osc1Wave = 0; lead.osc2Wave = 2; lead.osc1Level = 0.8f; lead.osc2Level = 0.6f;
        lead.cutoffHz = 3000.0f; lead.resonance = 0.4f; lead.filterEnvAmt = 0.5f; lead.ampS = 0.9f;
        for (int i = 0; i < 3; ++i) engine.noteOn (60 + i, 0.8f, 0);         // lead (part 0)
        for (int i = 0; i < 4; ++i) engine.noteOn (48 + i * 3, 0.7f, 1);     // pad chord (part 1, unison x3)
        engine.noteOn (28, 0.9f, 2); engine.noteOn (35, 0.8f, 2);           // bass (part 2)
        for (int i = 0; i < 12; ++i) engine.kitNoteOn (3, 40 + i, 0.8f);     // kit (part 3, all pads)

        std::vector<float> L (kBlock, 0.0f), R (kBlock, 0.0f);
        for (int i = 0; i < 200; ++i) engine.renderMaster (L.data(), R.data(), kBlock, lead, lfo.data(), fx.data());
        std::vector<double> times ((std::size_t) blocks);
        for (int b = 0; b < blocks; ++b)
        {
            const auto t0 = clk::now();
            engine.renderMaster (L.data(), R.data(), kBlock, lead, lfo.data(), fx.data());
            const auto t1 = clk::now();
            times[(std::size_t) b] = std::chrono::duration<double, std::milli> (t1 - t0).count();
        }
        std::sort (times.begin(), times.end());
        return { times[(std::size_t) (blocks / 2)], times[(std::size_t) (int) (blocks * 0.99)], times.back() };
    }

    // Per-part MOD MATRIX / block-mod load: every active part carries a full 8-slot matrix mixing
    // voice-tier (cutoff/reso), block-tier (FX/EQ) and mixer-tier (PartLevel/PartPan) routes — the
    // path added by the LINK P0 / PartLevel / per-part block-mod work this cycle. Measures its cost
    // on top of a 4-part, all-FX, 3-LFO render (compare against the live-set row, which carries none).
    Stat measureMatrixLoad (int blocks)
    {
        SynthEngine engine;
        engine.setOscQuality (PolyBlepOscillator::Quality::Efficient);
        engine.setMaxVoices (24);
        engine.prepare (kSR, kBlock);

        ModMatrix mtx;
        mtx.slots[0] = { ModMatrix::LFO1, ModMatrix::Cutoff,    0.6f };
        mtx.slots[1] = { ModMatrix::LFO1, ModMatrix::Resonance, 0.4f };
        mtx.slots[2] = { ModMatrix::LFO2, ModMatrix::ReverbMix, 0.5f };
        mtx.slots[3] = { ModMatrix::LFO2, ModMatrix::ChorusMix, 0.5f };
        mtx.slots[4] = { ModMatrix::LFO3, ModMatrix::DelayMix,  0.4f };
        mtx.slots[5] = { ModMatrix::LFO3, ModMatrix::EqB3Gain,  0.5f };
        mtx.slots[6] = { ModMatrix::LFO1, ModMatrix::PartLevel, 0.5f };   // mixer-tier tremolo
        mtx.slots[7] = { ModMatrix::LFO2, ModMatrix::PartPan,   0.5f };   // mixer-tier auto-pan

        VoiceParams vp;
        vp.osc1Level = 0.8f; vp.osc2Level = 0.8f; vp.osc3Level = 0.6f;
        vp.cutoffHz = 2000.0f; vp.resonance = 0.4f; vp.filterEnvAmt = 0.5f; vp.ampS = 0.9f;

        std::array<FXParams, SynthEngine::maxParts> fx {};
        std::array<PartLfos, SynthEngine::maxParts> lfo {};
        engine.setLiveModMatrix (mtx);                                    // part 0 (live/focused)
        for (int p = 0; p < SynthEngine::maxParts; ++p)
        {
            for (int f = 0; f < FXChain::kNumFX; ++f) fx[(std::size_t) p].enabled[(std::size_t) f] = true;
            for (int k = 0; k < 3; ++k) lfo[(std::size_t) p].lfo[k] = { 3.0f + (float) k, 0.5f, 0, 2 };   // all 3 LFOs live
            if (p >= 1) engine.setLockedPartParams (p, vp, fx[(std::size_t) p], lfo[(std::size_t) p], mtx);
            for (int v = 0; v < 4; ++v) engine.noteOn (36 + p * 12 + v, 0.7f, p);
        }

        std::vector<float> L (kBlock, 0.0f), R (kBlock, 0.0f);
        for (int i = 0; i < 200; ++i) engine.renderMaster (L.data(), R.data(), kBlock, vp, lfo.data(), fx.data());
        std::vector<double> times ((std::size_t) blocks);
        for (int b = 0; b < blocks; ++b)
        {
            const auto t0 = clk::now();
            engine.renderMaster (L.data(), R.data(), kBlock, vp, lfo.data(), fx.data());
            const auto t1 = clk::now();
            times[(std::size_t) b] = std::chrono::duration<double, std::milli> (t1 - t0).count();
        }
        std::sort (times.begin(), times.end());
        return { times[(std::size_t) (blocks / 2)], times[(std::size_t) (int) (blocks * 0.99)], times.back() };
    }

    void row (const std::string& label, Stat s)
    {
        const double tpP99 = s.p99Ms * kTargetDerate;      // robust worst-case, derated
        const double tpPct = tpP99   / kBudgetMs * 100.0;
        std::printf ("  %-22s  p50 %6.3f  p99 %6.3f  max %6.3f ms  | "
                     "target~ p99 %6.3f ms (%5.1f%% budget)  %s\n",
                     label.c_str(), s.medMs, s.p99Ms, s.maxMs, tpP99, tpPct,
                     tpPct < 30.0 ? "OK<30%" : (tpPct < 100.0 ? "runs" : "OVERRUN"));
    }
}

int main()
{
    std::printf ("VA Synth block benchmark @ 48 kHz, 128-sample block "
                 "(budget %.3f ms)\n", kBudgetMs);
    std::printf ("Worst-case = saw+saw, per-sample filter-env cutoff mod. Runs ON the reference "
                 "target (i7-8650U); target~ = measured x%.1f (no dev->target derate).\n\n", kTargetDerate);

    struct Qc { const char* name; PolyBlepOscillator::Quality q; };
    const Qc modes[] {
        { "(a) None 1x (raw)",   PolyBlepOscillator::Quality::None },
        { "(b) Efficient 4x",    PolyBlepOscillator::Quality::Efficient },
        { "(c) HQ 4x/320",       PolyBlepOscillator::Quality::HQ },
    };

    std::printf ("16 voices (full pool):\n");
    for (auto m : modes) row (m.name, measure (m.q, 16, 4000));

    std::printf ("\n12 voices (comparison; live cap is now 16 for split voicing):\n");
    for (auto m : modes) row (m.name, measure (m.q, 12, 4000));

    std::printf ("\n1 voice:\n");
    for (auto m : modes) row (m.name, measure (m.q, 1, 4000));

    // 6A gate: 12 voices, Efficient, osc-count sweep (kill-switch savings).
    std::printf ("\n6A osc-count (12 voices, Efficient, full-level saws):\n");
    row ("1 osc on", measure (PolyBlepOscillator::Quality::Efficient, 12, 4000, 1));
    row ("2 osc on", measure (PolyBlepOscillator::Quality::Efficient, 12, 4000, 2));
    row ("3 osc on", measure (PolyBlepOscillator::Quality::Efficient, 12, 4000, 3));

    // 6B gate: full path (3-osc/12-voice Efficient engine + FX), FX cost sweep.
    std::printf ("\n6B full path (12 voices, 3 osc, Efficient + FX):\n");
    row ("engine only",         measureFull (12, 4000, 3, 0));
    row ("+ chorus",            measureFull (12, 4000, 3, 1));
    row ("+ chorus (2 voices)", measureFull (12, 4000, 3, 1, 0.0f, 2));   // Tier 4c delta (second tap)
    row ("+ delay",             measureFull (12, 4000, 3, 2));
    row ("+ reverb",            measureFull (12, 4000, 3, 4));
    row ("+ reverb (motion)",   measureFull (12, 4000, 3, 4, 1.0f));   // Tier 4a delta (3 modulated allpass)
    row ("+ width",             measureFull (12, 4000, 3, 8));
    row ("+ width (SAT)",       measureFull (12, 4000, 3, 8, 0.0f, 1, 1.0f));   // FX SAT delta (tube saturation)
    row ("+ ALL FX",            measureFull (12, 4000, 3, 15));

    std::printf ("\n6B full path (16 voices, 3 osc, Efficient + FX):\n");
    row ("16v engine only",     measureFull (16, 4000, 3, 0));
    row ("16v + ALL FX",        measureFull (16, 4000, 3, 15));

    // Pool bump to 24 (multitimbral headroom: seq kit + looper patch + lead + spare).
    // Worst case = all 24 voices sounding, saw+saw, per-sample cutoff mod, + ALL FX.
    std::printf ("\n24-voice pool (raised for multitimbral, 3 osc, Efficient + FX):\n");
    row ("24v engine only",     measureFull (24, 4000, 3, 0));
    row ("24v + ALL FX",        measureFull (24, 4000, 3, 15));
    // A realistic heavy patch: fewer voices but lush FX (pad territory).
    std::printf ("\n6B realistic pad (6 voices, 3 osc, chorus+reverb):\n");
    row ("6 voice + cho+rev",   measureFull (6, 4000, 3, 1 | 4));

    // 2C: filter DRIVE puts every voice on the 2x oversampled path (worst case for the driven
    // filter). Compare clean vs driven to see the oversampling cost; the drive=0 fast path means
    // ordinary patches pay nothing. This is the number the minimum-spec target validation (#100) watches.
    std::printf ("\n2C driven filter (2x oversampled path, 3 osc):\n");
    row ("16v clean (drive 0)",  measure (PolyBlepOscillator::Quality::Efficient, 16, 4000, 3, 0.0f));
    row ("16v driven (drive 1)", measure (PolyBlepOscillator::Quality::Efficient, 16, 4000, 3, 1.0f));
    row ("24v driven (drive 1)", measure (PolyBlepOscillator::Quality::Efficient, 24, 4000, 3, 1.0f));

    // Sub-phase 1 gate: kit worst case (live chord + 12 sustained pads = 16 voices,
    // 12 through the Kit paramsFor branch), engine-only and + ALL FX.
    std::printf ("\nSub-phase 1 kit (4 live + 12 kit pads = 16 voices, Efficient):\n");
    row ("kit engine only",     measureKit (4000, 0));
    row ("kit + ALL FX",        measureKit (4000, 15));

    // Sub-phase 2 gate: full multitimbral. Worst case = 4 active parts, all with their
    // own FX, 16 voices spread, Efficient. Plus the realistic case (2 parts, FX on one).
    std::printf ("\nSub-phase 2 multitimbral (Efficient, per-part FX):\n");
    row ("4 parts x4v + 4x ALL FX", measureMulti (4, 4, 4, 4000));   // hard-gate worst case
    row ("2 parts x8v + 1x ALL FX", measureMulti (2, 8, 1, 4000));   // realistic case
    row ("1 part 16v + ALL FX",     measureMulti (1, 16, 1, 4000));  // single-part reference

    // #96 UNISON (never benched before): live cap under Efficient is 3; the pre-agreed 7-member
    // contingency needs HQ. Cost = notes x members x oscs — a lush supersaw chord, + ALL FX.
    using Q = PolyBlepOscillator::Quality;
    std::printf ("\n#96 unison (renderMaster, + ALL FX):\n");
    {
        VoiceParams u; u.osc1Wave = u.osc2Wave = u.osc3Wave = 0;
        u.osc1Level = u.osc2Level = u.osc3Level = 0.8f; u.cutoffHz = 2000.0f; u.resonance = 0.4f;
        u.filterEnvAmt = 0.5f; u.ampS = 0.9f; u.unisonDetune = 0.2f; u.unisonWidth = 0.6f;
        u.unisonCount = 3; row ("Eff x3 (live cap) 12 notes",  measureLocked (Q::Efficient, u, 12, true, 4000));
        u.unisonCount = 7; row ("HQ  x7 (contingency) 8 notes", measureLocked (Q::HQ,        u,  8, true, 4000));
    }

    // #132 FM under polyphony (newest DSP — never measured anywhere): BOTH FM depths active, sine
    // carriers (FM applies only to sin/tri/WT), full pool + ALL FX.
    std::printf ("\n#132 FM under polyphony (both depths, sine carriers, + ALL FX):\n");
    {
        VoiceParams f; f.osc1Wave = f.osc2Wave = f.osc3Wave = 3;   // 3 = sine
        f.osc1Level = f.osc2Level = f.osc3Level = 0.8f; f.osc1Fm = 0.8f; f.osc2Fm = 0.8f;
        f.cutoffHz = 2000.0f; f.resonance = 0.4f; f.filterEnvAmt = 0.5f; f.ampS = 0.9f;
        row ("16 notes FM", measureLocked (Q::Efficient, f, 16, true, 4000));
        row ("24 notes FM", measureLocked (Q::Efficient, f, 24, true, 4000));
    }

    // 2C rider: sustained pedaled chords on a HEAVILY DRIVEN patch + a SELF-OSCILLATING filter —
    // the always-oversample-when-driven decision's real-world worst case.
    std::printf ("\n2C driven + self-oscillating (drive 1, reso ~self-osc, sustained, + ALL FX):\n");
    {
        VoiceParams d; d.osc1Wave = d.osc2Wave = d.osc3Wave = 0;
        d.osc1Level = d.osc2Level = d.osc3Level = 0.8f; d.cutoffHz = 1500.0f; d.resonance = 0.98f;   // self-osc
        d.filterEnvAmt = 0.5f; d.drive = 1.0f; d.ampS = 1.0f;   // fully sustained (pedaled)
        row ("16 notes driven+selfosc", measureLocked (Q::Efficient, d, 16, true, 4000));
        row ("24 notes driven+selfosc", measureLocked (Q::Efficient, d, 24, true, 4000));
    }

    // The realistic LIVE SET the zero-xrun ship rule keys on (2-3 synth parts + a sequenced kit +
    // per-part FX + synced LFOs + looper-density notes). If THIS is clean, we ship as configured.
    std::printf ("\nRealistic live set (lead + unison pad + FM/driven bass + sequenced kit, all FX + LFOs):\n");
    row ("live set (4 parts)", measureLiveSet (4000));

    std::printf ("\nPer-part mod matrix + block-mods (4 parts, full 8-slot matrix: LFO->cutoff/reso, FX/EQ, PartLevel/PartPan, all FX + 3 LFOs):\n");
    row ("matrix load (4 parts)", measureMatrixLoad (4000));

    std::printf ("\nBudget = 2.667 ms/block. Target: worst-case target < 30%% "
                 "leaves headroom for GUI, other tracks, and OS jitter.\n");
    return 0;
}
