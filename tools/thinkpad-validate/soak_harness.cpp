// ============================================================================
// ThinkPad soak harness (JUCE-free). Runs the real DSP path (SynthEngine +
// FXChain, exactly as the processor does per block) flat-out for a wall-clock
// duration under a synthetic MIDI storm, with ALL FX engaged and the voice pool
// kept saturated. This is the portable proxy for a device xrun test: it counts
// COMPUTE OVERRUNS (blocks whose render time exceeds the real-time budget), which
// is what would cause an xrun at that buffer size, and it stresses the CPU
// (thermal throttling) for the full duration. It needs no audio device, no MIDI
// port and no JUCE — just a C++17 compiler — so it runs anywhere.
//
//   build:  g++ -O3 -march=native -std=c++17 soak_harness.cpp -Idsp -o soak -lpthread
//   run:    ./soak [seconds] [blockSize]      (defaults: 600, 128)
// ============================================================================
#include "SynthEngine.h"
#include "FXChain.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <cmath>

using clk = std::chrono::steady_clock;

int main (int argc, char** argv)
{
    const double seconds   = (argc > 1) ? std::atof (argv[1]) : 600.0;
    const int    block     = (argc > 2) ? std::atoi (argv[2]) : 128;
    const double SR        = 48000.0;
    const double budgetMs  = block / SR * 1000.0;

    SynthEngine eng;
    eng.setOscQuality (PolyBlepOscillator::Quality::Efficient);   // the live-machine mode
    eng.setMaxVoices (16);
    eng.prepare (SR);

    // Worst-case voice: saw+saw+saw with per-sample filter-env cutoff modulation.
    VoiceParams p;
    p.osc1Wave = p.osc2Wave = p.osc3Wave = 0;
    p.osc1Level = p.osc2Level = p.osc3Level = 0.8f;
    p.cutoffHz = 2000.0f; p.resonance = 0.4f; p.filterEnvAmt = 0.5f;
    p.ampS = 0.9f;

    FXChain fx; fx.prepare (SR, block);
    FXParams fp;
    fp.enabled[FXChain::Chorus_] = fp.enabled[FXChain::Delay_] =
    fp.enabled[FXChain::Reverb_] = fp.enabled[FXChain::Width_] = true;    // ALL FX
    fp.chorusMix = 0.5f; fp.delayMix = 0.4f; fp.reverbMix = 0.4f; fp.width = 1.5f;
    fx.setParams (fp);

    // Saturate the pool, then churn note lifecycle (on/off/steal) over the run.
    for (int i = 0; i < 16; ++i) eng.noteOn (36 + i, 0.7f, 0);

    std::vector<float> mono ((size_t) block, 0.0f), L ((size_t) block, 0.0f), R ((size_t) block, 0.0f);
    for (int i = 0; i < 200; ++i)                                         // warm caches / settle FX crossfades
    { eng.render (mono.data(), block, p, 3.0f, 0, 0.3f, 2); std::copy (mono.begin(), mono.end(), L.begin());
      std::copy (mono.begin(), mono.end(), R.begin()); fx.process (L.data(), R.data(), block); }

    std::vector<double> sample; sample.reserve (1u << 20);               // every 64th block, for percentiles
    double maxMs = 0.0, sumMs = 0.0;
    long long blocks = 0, overruns = 0, evt = 0;
    const auto tStart = clk::now();
    auto lastProgress = tStart;

    for (;;)
    {
        const auto now = clk::now();
        const double elapsed = std::chrono::duration<double> (now - tStart).count();
        if (elapsed >= seconds) break;

        // Synthetic MIDI storm: every 16 blocks retire a note and trigger another,
        // exercising note-on / note-off / oldest-note stealing continuously.
        if (blocks % 16 == 0)
        {
            eng.noteOff (36 + (int) (evt % 24), 0);
            eng.noteOn  (36 + (int) ((evt * 7 + 3) % 24), 0.7f, 0);
            ++evt;
        }

        const auto b0 = clk::now();
        eng.render (mono.data(), block, p, 3.0f, 0, 0.3f, 2);
        std::copy (mono.begin(), mono.end(), L.begin());
        std::copy (mono.begin(), mono.end(), R.begin());
        fx.process (L.data(), R.data(), block);
        const auto b1 = clk::now();

        const double ms = std::chrono::duration<double, std::milli> (b1 - b0).count();
        if (ms > budgetMs) ++overruns;
        if (ms > maxMs) maxMs = ms;
        sumMs += ms;
        if ((blocks & 63) == 0) sample.push_back (ms);
        ++blocks;

        if (std::chrono::duration<double> (now - lastProgress).count() >= 30.0)
        {
            std::fprintf (stderr, "  soak %4.0f/%.0fs  blocks=%lld  overruns=%lld  max=%.3fms\n",
                          elapsed, seconds, blocks, overruns, maxMs);
            lastProgress = now;
        }
    }

    std::sort (sample.begin(), sample.end());
    auto pct = [&] (double q) { return sample.empty() ? 0.0 : sample[(size_t) std::min (sample.size() - 1, (size_t) (q * sample.size())) ]; };
    const double wall = std::chrono::duration<double> (clk::now() - tStart).count();
    const double blockRealMs = block / SR * 1000.0;
    const double rtFactor = (blocks * blockRealMs) / (wall * 1000.0);    // audio-time / wall-time

    std::printf ("soak: %.0f s wall, block=%d (%.3f ms budget)\n", wall, block, budgetMs);
    std::printf ("  blocks rendered : %lld  (%.1f min of audio, %.1fx real-time flat-out)\n",
                 blocks, blocks * blockRealMs / 60000.0, rtFactor);
    std::printf ("  compute/block   : mean %.4f  p50 %.4f  p99 %.4f  max %.4f ms\n",
                 blocks ? sumMs / (double) blocks : 0.0, pct (0.50), pct (0.99), maxMs);
    std::printf ("  COMPUTE OVERRUNS: %lld  (blocks over the %.3f ms budget = %.4f%%)\n",
                 overruns, budgetMs, blocks ? 100.0 * (double) overruns / (double) blocks : 0.0);
    std::printf ("  verdict         : %s\n",
                 overruns == 0 ? "PASS (no block exceeded budget in the whole soak)"
                               : "OVERRUNS PRESENT — inspect max/p99 and thermal context above");
    return 0;
}
