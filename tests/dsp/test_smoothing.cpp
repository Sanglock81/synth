// ============================================================================
// Parameter-smoothing tests (zipper noise). Written test-first.
//
// Finding: cutoff stepping through the TPT SVF does not produce click-magnitude
// per-sample discontinuities (the filter is graceful with coefficient changes).
// But under a coarse knob/automation staircase the cutoff still jumps once per
// block, which is audible. We smooth it and verify with a HIGH-FREQUENCY PROBE
// TONE: a ~6 kHz sine through the LP. Its amplitude tracks the *effective*
// (smoothed) cutoff, so a hard cutoff step reveals whether the change is instant
// (unsmoothed) or ramped (smoothed). Master gain is covered at the plugin layer.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "SynthEngine.h"
#include "test_util.h"
#include <vector>
#include <cmath>

namespace
{
    constexpr double kSR = 48000.0;

    double rmsWindow (const std::vector<float>& x, int start, int len)
    {
        double acc = 0.0;
        for (int i = 0; i < len; ++i) { float s = x[(std::size_t)(start + i)]; acc += double (s) * s; }
        return std::sqrt (acc / len);
    }
}

TEST_CASE ("cutoff is smoothed: a hard step ramps rather than jumping instantly",
           "[smoothing][zipper][cutoff]")
{
    const int block = 64, blocks = 200, stepBlock = 100;
    const int stepSample = block * stepBlock;

    SynthEngine e; e.prepare (kSR);
    VoiceParams p;
    p.osc1Wave = 3; p.osc2Wave = 3; p.oscMix = 0.0f;      // pure sine probe tone
    p.noiseLevel = 0.0f; p.filterEnvAmt = 0.0f; p.resonance = 0.0f;
    p.ampA = 0.001f; p.ampD = 0.01f; p.ampS = 1.0f; p.ampR = 0.1f;
    e.noteOn (114, 0.8f);                                  // ~5.9 kHz probe

    std::vector<float> out (block * blocks, 0.0f);
    for (int b = 0; b < blocks; ++b)
    {
        VoiceParams q = p;
        q.cutoffHz = (b < stepBlock) ? 18000.0f : 400.0f;  // wide open -> closed
        e.render (out.data() + b * block, block, q, 2.0f, 0, 0.0f, 0);
    }

    const int ms2  = int (kSR * 0.002);
    const int ms25 = int (kSR * 0.025);
    const double before    = rmsWindow (out, stepSample - 256, 128);
    const double justAfter = rmsWindow (out, stepSample + ms2, 128);   // ~2 ms after
    const double wellAfter = rmsWindow (out, stepSample + ms25, 128);  // ~25 ms after

    INFO ("probe RMS before=" << before << " +2ms=" << justAfter << " +25ms=" << wellAfter);
    REQUIRE (before > 0.05);                          // probe audible before the step
    REQUIRE (justAfter > 0.5 * before);               // smoothed: still open ~2 ms later
    REQUIRE (wellAfter < 0.2 * before);               // fully closed by ~25 ms
}
