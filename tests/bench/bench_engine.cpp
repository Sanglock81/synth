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
    Stat measure (PolyBlepOscillator::Quality q, int voices, int blocks)
    {
        SynthEngine engine;
        engine.setOscQuality (q);
        engine.prepare (kSR);

        VoiceParams p;                       // saw/saw, worst case (oversampled)
        p.cutoffHz = 2000.0f; p.resonance = 0.4f; p.filterEnvAmt = 0.5f;
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

    std::printf ("16 voices (full polyphony):\n");
    for (auto m : modes) row (m.name, measure (m.q, 16, 4000));

    std::printf ("\n1 voice:\n");
    for (auto m : modes) row (m.name, measure (m.q, 1, 4000));

    std::printf ("\nBudget = 2.667 ms/block. Target: worst-case ThinkPad < 30%% "
                 "leaves headroom for GUI, other tracks, and OS jitter.\n");
    return 0;
}
