// ============================================================================
// DSP performance benchmark (JUCE-free). Measures worst-case 128-sample block
// render time at 48 kHz for the full engine, across oscillator quality modes,
// so we can keep VA Synth glitch-free on modest hardware (2-core Broadwell
// ThinkPad X1 Carbon 3rd gen, the live machine).
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
    // Conservative derating: this dev machine (Ryzen 7) is ~3.5x faster single-
    // thread than the 2015 Broadwell ThinkPad. Scale measured times up by this.
    constexpr double kThinkpadDerate = 3.5;

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
    Stat measureFull (int voices, int blocks, int oscsOn, int fxMask)
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

    void row (const std::string& label, Stat s)
    {
        const double tpP99 = s.p99Ms * kThinkpadDerate;      // robust worst-case, derated
        const double tpPct = tpP99   / kBudgetMs * 100.0;
        std::printf ("  %-22s  p50 %6.3f  p99 %6.3f  max %6.3f ms  | "
                     "ThinkPad~ p99 %6.3f ms (%5.1f%% budget)  %s\n",
                     label.c_str(), s.medMs, s.p99Ms, s.maxMs, tpP99, tpPct,
                     tpPct < 30.0 ? "OK<30%" : (tpPct < 100.0 ? "runs" : "OVERRUN"));
    }
}

int main()
{
    std::printf ("VA Synth block benchmark @ 48 kHz, 128-sample block "
                 "(budget %.3f ms)\n", kBudgetMs);
    std::printf ("Worst-case = saw+saw, per-sample filter-env cutoff mod. "
                 "ThinkPad~ = measured x%.1f.\n\n", kThinkpadDerate);

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
    row ("+ delay",             measureFull (12, 4000, 3, 2));
    row ("+ reverb",            measureFull (12, 4000, 3, 4));
    row ("+ width",             measureFull (12, 4000, 3, 8));
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
    // ordinary patches pay nothing. This is the number the ThinkPad validation (#100) watches.
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

    std::printf ("\nBudget = 2.667 ms/block. Target: worst-case ThinkPad < 30%% "
                 "leaves headroom for GUI, other tracks, and OS jitter.\n");
    return 0;
}
