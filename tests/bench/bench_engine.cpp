// ============================================================================
// DSP performance benchmark (JUCE-free). Reports how much CPU the engine needs
// so we can keep VA Synth runnable on modest hardware. Not a CTest gate — wall
// time is machine-dependent — but run it before/after DSP changes to watch the
// budget. Prints xRealtime and single-core CPU% for continuous full polyphony.
//
//   build:  cmake --build build --target dsp_bench
//   run:    ./build/tests/dsp_bench
// ============================================================================
#include "SynthEngine.h"
#include <chrono>
#include <cstdio>
#include <vector>
#include <string>

namespace
{
    using clk = std::chrono::steady_clock;
    constexpr double kSR = 48000.0;

    struct Result { double xRealtime, cpuPct, nsPerSample; };

    // Render `audioSeconds` of audio with `numVoices` held, in `block`-sample
    // chunks, and time it. Returns throughput metrics.
    Result run (VoiceParams p, int numVoices, int block, double audioSeconds)
    {
        SynthEngine engine;
        engine.prepare (kSR);
        for (int i = 0; i < numVoices; ++i)
            engine.noteOn (36 + i, 0.7f);

        const int totalSamples = int (kSR * audioSeconds);
        std::vector<float> out (block, 0.0f);

        // warm up
        for (int i = 0; i < 20; ++i)
            engine.render (out.data(), block, p, 3.0f, 0, 0.3f, 2);

        const auto t0 = clk::now();
        int done = 0;
        while (done < totalSamples)
        {
            const int n = std::min (block, totalSamples - done);
            engine.render (out.data(), n, p, 3.0f, 0, 0.3f, 2);
            done += n;
        }
        const auto t1 = clk::now();

        const double wall = std::chrono::duration<double> (t1 - t0).count();
        const double xrt  = audioSeconds / wall;
        return { xrt, 100.0 / xrt, wall / totalSamples * 1e9 };
    }

    VoiceParams sawParams()
    {
        VoiceParams p;                       // default saw/saw + 2 kHz LP
        p.cutoffHz = 2000.0f; p.resonance = 0.3f; p.filterEnvAmt = 0.4f;
        return p;
    }

    void report (const std::string& label, const Result& r, int voices)
    {
        std::printf ("  %-28s  %8.1fx RT   %6.2f%% CPU   %6.2f ns/smp   %6.3f ns/voice-smp\n",
                     label.c_str(), r.xRealtime, r.cpuPct, r.nsPerSample,
                     r.nsPerSample / voices);
    }
}

int main()
{
    std::printf ("VA Synth engine benchmark @ %.0f kHz, 128-sample blocks\n", kSR / 1000.0);
    std::printf ("(xRT = times faster than real-time; CPU%% = one core, continuous)\n\n");

    auto p = sawParams();
    std::printf ("Full signal chain (saw+saw -> SVF LP w/ per-sample cutoff mod -> VCA):\n");
    report ("16 voices (full poly)", run (p, 16, 128, 5.0), 16);
    report ("8 voices",             run (p,  8, 128, 5.0), 8);
    report ("1 voice",              run (p,  1, 128, 5.0), 1);

    VoiceParams sine = p; sine.osc1Wave = 3; sine.osc2Wave = 3;
    std::printf ("\nSine oscillators (isolates filter+env+mix cost from PolyBLEP):\n");
    report ("16 voices sine",       run (sine, 16, 128, 5.0), 16);

    std::printf ("\nInterpretation: full-poly CPU%% is the headline number. Multiple\n"
                 "instances / other plugins share the core, so keep it low.\n");
    return 0;
}
