// Synth — Copyright © 2026 John L Farmer. Licensed under AGPLv3; see LICENSE.
// ============================================================================
// #95 Wavetable (3b) — the DETERMINISTIC content generator. Two pins:
//  (1) equal-loudness normalization is ONE shared step for factory + randomizer;
//  (2) seed->table is BIT-IDENTICAL across platforms (persisted by seed, so a
//      preset must regenerate the same bytes everywhere or it is a silent fork).
// The determinism golden below (fixed content hashes) is checked by CI on Windows.
// JUCE-free.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "WavetableGen.h"
#include <vector>
#include <cmath>
#include <complex>

namespace
{
    constexpr double kSR = 48000.0;

    double rmsAt (const Wavetable& wt, double f0, float pos, int N = 8192)
    {
        std::vector<float> b ((std::size_t) N); double ph = 0.0; const int mip = wt.mipForFreq (f0);
        for (auto& v : b) { v = wt.read (ph, pos, mip); ph += f0 / kSR; if (ph >= 1.0) ph -= 1.0; }
        double e = 0.0; for (float v : b) e += (double) v * v; return std::sqrt (e / (double) N);
    }

    double aliasFraction (const Wavetable& wt, double f0, float pos, int N = 16384)
    {
        std::vector<float> b ((std::size_t) N); double ph = 0.0; const int mip = wt.mipForFreq (f0);
        for (auto& v : b) { v = wt.read (ph, pos, mip); ph += f0 / kSR; if (ph >= 1.0) ph -= 1.0; }
        auto mag = [&] (double hz) { double re = 0, im = 0;
            for (int n = 0; n < N; ++n) { const double a = 6.283185307179586 * hz * n / kSR; re += b[(std::size_t) n] * std::cos (a); im -= b[(std::size_t) n] * std::sin (a); }
            return std::sqrt (re * re + im * im) / N; };
        double harm = 0.0, alias = 0.0;
        for (int k = 1; k * f0 < kSR * 0.5; ++k) { const double m = mag (k * f0); harm += m * m; }
        for (int k = 1; k * f0 < kSR * 0.5; ++k) { const double f = (k + 0.5) * f0; if (f < kSR * 0.5) { const double m = mag (f); alias += m * m; } }
        return (harm + alias) > 0.0 ? alias / (harm + alias) : 0.0;
    }
}

TEST_CASE ("WavetableGen: seed and factory tables regenerate BIT-IDENTICALLY (cross-platform golden)",
           "[dsp][wavetable][gen][golden]")
{
    // Committed content hashes (FNV-1a of the raw table bytes). These were captured on Linux and are
    // identical at -O0 / -O2 / -O3 -march=native (FMA on) because the generator is compiled with FP
    // contraction OFF. If CI on Windows (MSVC) computes a different hash, a preset shared across
    // platforms would regenerate a DIFFERENT table -> a silent content fork; this test catches it.
    REQUIRE (wtgen::buildFactory (0, kSR).contentHash() == 0xcae86f7760dd3a47ull);   // Analog
    REQUIRE (wtgen::buildFactory (1, kSR).contentHash() == 0xa1b9cbf3b36ca763ull);   // Sweep
    REQUIRE (wtgen::buildFactory (2, kSR).contentHash() == 0xab947e9ec2632393ull);   // Vowel
    REQUIRE (wtgen::buildFactory (3, kSR).contentHash() == 0xfc795ba96f3b864bull);   // Digital
    REQUIRE (wtgen::buildRandom (42u, kSR).contentHash() == 0x6311ca24b407e136ull);
    REQUIRE (wtgen::buildRandom (7u,  kSR).contentHash() == 0xf6a2dd8cd0d6f106ull);
}

TEST_CASE ("WavetableGen: a seed regenerates identically within a run (repeatable randomizer)",
           "[dsp][wavetable][gen]")
{
    for (std::uint32_t seed : { 1u, 42u, 999u })
        REQUIRE (wtgen::buildRandom (seed, kSR).contentHash() == wtgen::buildRandom (seed, kSR).contentHash());
    // ...and different seeds give different tables.
    REQUIRE (wtgen::buildRandom (1u, kSR).contentHash() != wtgen::buildRandom (2u, kSR).contentHash());
}

TEST_CASE ("WavetableGen: factory tables and randomizer share the equal-RMS normalization",
           "[dsp][wavetable][gen][loudness]")
{
    // Switching tables or re-rolling the die must not jump the level: every table (factory + several
    // seeds), read at the same pitch/position, lands within a tight loudness window.
    double lo = 1.0e9, hi = 0.0;
    auto note = [&] (const Wavetable& wt) { const double r = rmsAt (wt, 220.0, 0.5f); lo = std::min (lo, r); hi = std::max (hi, r); };
    for (int id = 0; id < wtgen::factoryCount(); ++id) note (wtgen::buildFactory (id, kSR));
    for (std::uint32_t s : { 3u, 17u, 5000u })                    note (wtgen::buildRandom (s, kSR));
    INFO ("RMS window lo=" << lo << " hi=" << hi << " ratio=" << hi / lo);
    REQUIRE (lo > 0.05);              // audible
    REQUIRE (hi / lo < 1.6);          // within ~4 dB across ALL tables (the shared normalize holds)
}

TEST_CASE ("WavetableGen: every factory table plays a high note alias-free", "[dsp][wavetable][gen]")
{
    for (int id = 0; id < wtgen::factoryCount(); ++id)
    {
        Wavetable wt = wtgen::buildFactory (id, kSR);
        REQUIRE (wt.valid());
        REQUIRE (wt.length() == wtgen::kFrameLen);
        for (float pos : { 0.0f, 0.5f, 1.0f })
        {
            INFO ("factory " << id << " (" << wtgen::factoryName (id) << ") pos " << pos);
            REQUIRE (aliasFraction (wt, 4186.0, pos) < 0.02);      // C8, mips keep it clean
        }
    }
}
