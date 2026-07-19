// ============================================================================
// J1.2 — tempo-synced LFOs. A synced LFO's rate is derived from the transport tempo +
// note division, and its phase is transport-position-derived (bar-locked, continuous).
// Measured via the engine's published focused-LFO raw output, sampled once per block.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "SynthEngine.h"
#include "test_util.h"
#include <vector>
#include <cmath>

namespace
{
    constexpr double kSR = 48000.0;
    constexpr int    kBlock = 256;
    constexpr double kBlockRate = kSR / kBlock;    // blocks/sec = the LFO sampling rate here

    // Run the engine with one synced LFO (dest cutoff, sine) at `bpm` for `seconds`, sampling the
    // focused LFO's raw output once per block. Returns the samples.
    std::vector<float> runSynced (int division, double bpm, double seconds,
                                  std::vector<float>* dense = nullptr)
    {
        SynthEngine eng; eng.prepare (kSR, kBlock);
        PartLfos pl; pl.lfo[0] = { 2.0f, 1.0f, 1 /*sine*/, 2 /*cutoff*/, true, division };
        const double spb = kSR * 60.0 / bpm;       // samples per beat
        double beats = 0.0;
        std::vector<float> out;
        std::vector<float> mL (kBlock), mR (kBlock);
        FXParams fx {};
        const int blocks = (int) (seconds * kBlockRate);
        eng.noteOn (60, 0.8f, 0, 0, false);        // a sustained voice so the LFO renders
        for (int b = 0; b < blocks; ++b)
        {
            eng.setTransport (beats, spb);
            eng.beginMasterBlock (kBlock, VoiceParams{}, fx, pl, 0);
            eng.renderParts (0, kBlock, VoiceParams{});
            eng.mixParts (mL.data(), mR.data(), kBlock);
            out.push_back (eng.focusLfoRawOut (0));
            if (dense) dense->push_back (eng.focusLfoRawOut (0));
            beats += (double) kBlock / spb;
        }
        return out;
    }
}

TEST_CASE ("synced LFO rate = tempo / division", "[dsp][lfo][sync][j1]")
{
    // @120 BPM: 1/4 = 2 Hz, 1/8 = 4 Hz, 1/16 = 8 Hz (division indices 4/5/6).
    REQUIRE (tu::zeroCrossHz (runSynced (4, 120.0, 3.0), 20, 400, kBlockRate) == Catch::Approx (2.0).margin (0.2));
    REQUIRE (tu::zeroCrossHz (runSynced (5, 120.0, 3.0), 20, 400, kBlockRate) == Catch::Approx (4.0).margin (0.3));
    REQUIRE (tu::zeroCrossHz (runSynced (6, 120.0, 3.0), 20, 400, kBlockRate) == Catch::Approx (8.0).margin (0.5));
}

TEST_CASE ("synced LFO follows a tempo change", "[dsp][lfo][sync][j1]")
{
    // Same 1/4 division at 90 vs 180 BPM -> 1.5 Hz vs 3 Hz (proves engine-side, live-tempo rate).
    REQUIRE (tu::zeroCrossHz (runSynced (4,  90.0, 4.0), 20, 500, kBlockRate) == Catch::Approx (1.5).margin (0.2));
    REQUIRE (tu::zeroCrossHz (runSynced (4, 180.0, 3.0), 20, 500, kBlockRate) == Catch::Approx (3.0).margin (0.3));
}

TEST_CASE ("triplet and dotted divisions rate correctly", "[dsp][lfo][sync][j1]")
{
    // @120: 1/4T (idx 8) = 3 Hz; 1/8. (idx 12, dotted eighth = 0.75 beat) = 2.667 Hz.
    REQUIRE (tu::zeroCrossHz (runSynced (8,  120.0, 3.0), 20, 400, kBlockRate) == Catch::Approx (3.0).margin (0.3));
    REQUIRE (tu::zeroCrossHz (runSynced (12, 120.0, 3.0), 20, 400, kBlockRate) == Catch::Approx (2.667).margin (0.3));
}

TEST_CASE ("synced LFO phase is continuous across the bar (no jump)", "[dsp][lfo][sync][j1]")
{
    // A 1/8. (dotted) division does NOT divide the bar evenly, so a reset-at-bar design would
    // step here. The transport-position derivation stays smooth. Sample past several bars.
    auto s = runSynced (12, 120.0, 5.0);   // 1/8. for 5 s (~2.5 bars @120)
    // Largest block-to-block step of a ~2.7 Hz sine sampled at 187.5 Hz is ~0.09; a bar-edge
    // discontinuity would spike well past that.
    REQUIRE (tu::maxDelta (s) < 0.2f);
    REQUIRE (tu::allFinite (s));
}
