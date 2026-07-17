// ============================================================================
// Mod matrix (#56) — the JUCE-free routing engine in isolation: inert by default
// (bit-identical guarantee), correct per-destination scaling, source selection,
// bipolar depth, and slot summing. RT-safe (fixed storage).
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "ModMatrix.h"
#include "SynthEngine.h"
#include <array>
#include <vector>

TEST_CASE ("mod matrix is inert by default", "[dsp][modmatrix]")
{
    ModMatrix m;
    REQUIRE_FALSE (m.active());
    ModSources s; s.lfo[0] = 1.0f; s.velocity = 1.0f;
    const auto o = m.evaluate (s);
    REQUIRE (o.pitchSemis == 0.0f);
    REQUIRE (o.cutoffOct  == 0.0f);
    REQUIRE (o.reso == 0.0f);
    REQUIRE (o.pw == 0.0f);
    REQUIRE (o.amp == 0.0f);
}

TEST_CASE ("a slot with source/dest but zero depth stays inert", "[dsp][modmatrix]")
{
    ModMatrix m;
    m.slots[0] = { ModMatrix::LFO1, ModMatrix::Cutoff, 0.0f };
    REQUIRE_FALSE (m.active());
}

TEST_CASE ("LFO -> cutoff scales by depth and the cutoff range", "[dsp][modmatrix]")
{
    ModMatrix m;
    m.slots[0] = { ModMatrix::LFO1, ModMatrix::Cutoff, 0.5f };
    REQUIRE (m.active());

    ModSources s; s.lfo[0] = 1.0f;
    REQUIRE (m.evaluate (s).cutoffOct == Catch::Approx (0.5f * ModMatrix::kRangeCutoff));

    s.lfo[0] = -1.0f;                                    // bipolar source follows sign
    REQUIRE (m.evaluate (s).cutoffOct == Catch::Approx (-0.5f * ModMatrix::kRangeCutoff));
}

TEST_CASE ("negative depth inverts the modulation", "[dsp][modmatrix]")
{
    ModMatrix m;
    m.slots[0] = { ModMatrix::LFO1, ModMatrix::Pitch, -1.0f };
    ModSources s; s.lfo[0] = 1.0f;
    REQUIRE (m.evaluate (s).pitchSemis == Catch::Approx (-ModMatrix::kRangePitch));
}

TEST_CASE ("each source reads the right field", "[dsp][modmatrix]")
{
    ModSources s;
    s.velocity = 0.5f; s.modEnv = 0.25f; s.modWheel = 1.0f; s.macro[3] = 0.75f; s.random = -1.0f;

    auto oneSlot = [&] (int src, int dst) { ModMatrix m; m.slots[0] = { src, dst, 1.0f }; return m.evaluate (s); };

    REQUIRE (oneSlot (ModMatrix::Velocity, ModMatrix::Amp).amp      == Catch::Approx (0.5f  * ModMatrix::kRangeAmp));
    REQUIRE (oneSlot (ModMatrix::ModEnv,   ModMatrix::Cutoff).cutoffOct == Catch::Approx (0.25f * ModMatrix::kRangeCutoff));
    REQUIRE (oneSlot (ModMatrix::ModWheel, ModMatrix::Resonance).reso == Catch::Approx (1.0f  * ModMatrix::kRangeReso));
    REQUIRE (oneSlot (ModMatrix::Macro4,   ModMatrix::PulseWidth).pw == Catch::Approx (0.75f * ModMatrix::kRangePw));
    REQUIRE (oneSlot (ModMatrix::Random,   ModMatrix::Pitch).pitchSemis == Catch::Approx (-1.0f * ModMatrix::kRangePitch));
}

TEST_CASE ("multiple slots targeting one destination sum", "[dsp][modmatrix]")
{
    ModMatrix m;
    m.slots[0] = { ModMatrix::LFO1, ModMatrix::Cutoff, 0.5f };
    m.slots[1] = { ModMatrix::ModEnv, ModMatrix::Cutoff, 0.25f };
    ModSources s; s.lfo[0] = 1.0f; s.modEnv = 1.0f;
    REQUIRE (m.evaluate (s).cutoffOct
             == Catch::Approx ((0.5f + 0.25f) * ModMatrix::kRangeCutoff));
}

// ---- end-to-end through the engine master path (the live/focused part) --------------------

namespace
{
    constexpr double kSR = 48000.0;

    // Peak of the live part (part 0) rendered through the master path, optionally with a
    // one-slot matrix and a mod-wheel value.
    float enginePeak (const ModMatrix* m, float modWheel)
    {
        SynthEngine e; e.setMaxVoices (16); e.prepare (kSR, 128);
        if (m != nullptr) e.setLiveModMatrix (*m);
        e.setModWheel (modWheel);
        std::array<PartLfos, SynthEngine::maxParts> pl {};
        std::array<FXParams,  SynthEngine::maxParts> fx {};
        VoiceParams vp;                                   // default saw voice on the live part
        e.noteOn (60, 0.8f, 0);
        std::vector<float> L (128, 0.0f), R (128, 0.0f);
        float peak = 0.0f;
        for (int b = 0; b < 30; ++b)
        {
            std::fill (L.begin(), L.end(), 0.0f); std::fill (R.begin(), R.end(), 0.0f);
            e.renderMaster (L.data(), R.data(), 128, vp, pl.data(), fx.data());
            if (b >= 6) for (int i = 0; i < 128; ++i) peak = std::max (peak, std::abs (L[i]));
        }
        return peak;
    }
}

TEST_CASE ("a live matrix ModWheel->Amp modulates the part through the engine (#56)", "[dsp][modmatrix][engine]")
{
    const float off = enginePeak (nullptr, 1.0f);        // no matrix: mod wheel only does vibrato
    ModMatrix m; m.slots[0] = { ModMatrix::ModWheel, ModMatrix::Amp, 1.0f };
    const float on  = enginePeak (&m, 1.0f);             // ModWheel(1) -> Amp -> ampMul ~= 2x

    REQUIRE (off > 0.0f);
    REQUIRE (on  > off * 1.5f);
}

TEST_CASE ("an empty live matrix renders identically to no matrix (bit-identical)", "[dsp][modmatrix][engine]")
{
    ModMatrix empty;                                     // inert
    REQUIRE (enginePeak (&empty, 0.0f) == enginePeak (nullptr, 0.0f));
}
