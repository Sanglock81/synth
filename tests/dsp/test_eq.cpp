// ============================================================================
// Master parametric EQ: flat = bit-transparent, band gains land on target dB, and
// a boosted band actually raises a tone at its centre. JUCE-free (DSP-only).
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "ParametricEQ.h"
#include "PartEQ.h"
#include <vector>
#include <cmath>

namespace
{
    constexpr double kSR = 48000.0;
    ParametricEQ::Band flat (float f) { return { f, 0.0f, 0.9f }; }
    PartEQ::Band pflat (float f) { return { f, 0.0f, 0.9f }; }
}

TEST_CASE ("PartEQ flat (all 0 dB) is transparent", "[dsp][eq][parteq]")
{
    PartEQ eq; eq.prepare (kSR);
    eq.setBands (pflat (180), pflat (1000), pflat (5000), pflat (10000), pflat (14000));
    std::vector<float> L (512), R (512);
    for (int i = 0; i < 512; ++i) { L[(size_t) i] = 0.3f * std::sin (i * 0.11f); R[(size_t) i] = 0.2f * std::sin (i * 0.05f); }
    auto L0 = L, R0 = R;
    eq.process (L.data(), R.data(), 512);
    for (int i = 0; i < 512; ++i)
    {
        REQUIRE (L[(size_t) i] == Catch::Approx (L0[(size_t) i]).margin (1e-4));
        REQUIRE (R[(size_t) i] == Catch::Approx (R0[(size_t) i]).margin (1e-4));
    }
}

TEST_CASE ("PartEQ: each of the 5 bells lands on its target dB (in isolation)", "[dsp][eq][parteq]")
{
    // Test one boosted band at a time (the others flat) so adjacent bells never bleed
    // into the measurement — proves all 5 bands are independently parametric.
    const float freqs[5] { 200.0f, 1000.0f, 5000.0f, 10000.0f, 15000.0f };
    const float gains[5] { 9.0f, -9.0f, 6.0f, -6.0f, 5.0f };
    for (int active = 0; active < 5; ++active)
    {
        PartEQ::Band b[5];
        for (int i = 0; i < 5; ++i) b[i] = { freqs[i], i == active ? gains[i] : 0.0f, 2.0f, true };
        PartEQ eq; eq.prepare (kSR);
        eq.setBands (b[0], b[1], b[2], b[3], b[4]);
        INFO ("band " << active << " @ " << freqs[active] << " Hz");
        REQUIRE (eq.magnitudeDb (freqs[active]) == Catch::Approx (gains[active]).margin (0.7));
    }
}

TEST_CASE ("PartEQ: a band switched off contributes exactly unity (0 dB)", "[dsp][eq][parteq]")
{
    PartEQ eq; eq.prepare (kSR);
    // Band 2 has a big cut but is OFF -> must read flat at its centre; the ON bands still land.
    PartEQ::Band b1 { 200.0f,   6.0f, 2.0f, true  };
    PartEQ::Band b2 { 1000.0f, -18.0f, 2.0f, false };   // off: gain ignored
    PartEQ::Band b3 { 6000.0f,  0.0f, 0.9f, true  };
    PartEQ::Band b4 { 12000.0f, 0.0f, 0.9f, true  };
    PartEQ::Band b5 { 16000.0f, 0.0f, 0.9f, true  };
    eq.setBands (b1, b2, b3, b4, b5);
    REQUIRE (eq.magnitudeDb (1000.0) == Catch::Approx (0.0).margin (0.3));   // off band = transparent
    REQUIRE (eq.magnitudeDb (200.0)  == Catch::Approx (6.0).margin (0.6));   // on band still works
}

TEST_CASE ("EQ flat (all 0 dB) is transparent", "[dsp][eq]")
{
    ParametricEQ eq; eq.prepare (kSR);
    eq.setBands (flat (120), flat (500), flat (3000), flat (8000));

    // A mixed test signal.
    std::vector<float> L (512), R (512);
    for (int i = 0; i < 512; ++i)
    {
        L[(size_t) i] = 0.3f * std::sin (i * 0.11f) + 0.2f * std::sin (i * 0.37f);
        R[(size_t) i] = 0.25f * std::sin (i * 0.05f);
    }
    auto L0 = L, R0 = R;
    eq.process (L.data(), R.data(), 512);
    for (int i = 0; i < 512; ++i)
    {
        REQUIRE (L[(size_t) i] == Catch::Approx (L0[(size_t) i]).margin (1e-4));
        REQUIRE (R[(size_t) i] == Catch::Approx (R0[(size_t) i]).margin (1e-4));
    }
}

TEST_CASE ("EQ bell gain lands on target dB at its centre", "[dsp][eq]")
{
    ParametricEQ eq; eq.prepare (kSR);
    eq.setBands (flat (120), flat (500), { 3000.0f, 12.0f, 2.0f }, flat (8000));
    REQUIRE (eq.magnitudeDb (3000.0) == Catch::Approx (12.0).margin (0.5));

    eq.setBands (flat (120), flat (500), { 3000.0f, -12.0f, 2.0f }, flat (8000));
    REQUIRE (eq.magnitudeDb (3000.0) == Catch::Approx (-12.0).margin (0.5));

    // Far from the bell it's ~untouched.
    REQUIRE (eq.magnitudeDb (100.0) == Catch::Approx (0.0).margin (0.5));
}

TEST_CASE ("EQ low shelf lifts lows, leaves highs", "[dsp][eq]")
{
    ParametricEQ eq; eq.prepare (kSR);
    eq.setBands ({ 200.0f, 6.0f, 0.7f }, flat (500), flat (3000), flat (8000));
    REQUIRE (eq.magnitudeDb (40.0)    == Catch::Approx (6.0).margin (1.0));
    REQUIRE (eq.magnitudeDb (12000.0) == Catch::Approx (0.0).margin (0.5));
}

TEST_CASE ("EQ boost raises a tone at the band centre", "[dsp][eq]")
{
    auto rms = [] (const std::vector<float>& v)
    {
        double s = 0; for (float x : v) s += (double) x * x;
        return std::sqrt (s / v.size());
    };
    const int N = 4096;
    std::vector<float> a (N), b (N), dummy (N, 0.0f);
    for (int i = 0; i < N; ++i) a[(size_t) i] = b[(size_t) i] = (float) std::sin (2.0 * eqconst::kPi * 1000.0 * i / kSR);
    const double before = rms (a);

    ParametricEQ eq; eq.prepare (kSR);
    eq.setBands (flat (120), { 1000.0f, 12.0f, 1.5f }, flat (3000), flat (8000));
    auto dummy2 = dummy;
    eq.process (b.data(), dummy2.data(), N);
    // skip the filter warm-up when measuring
    std::vector<float> tail (b.begin() + 1024, b.end());
    REQUIRE (rms (tail) > before * 1.5);
}
