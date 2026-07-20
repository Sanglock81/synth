// ============================================================================
// Filter tests: stability under sustained noise across the full parameter grid,
// frequency-response sanity, and behaviour under a full-range cutoff sweep.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "SVFilter.h"
#include "test_util.h"
#include <vector>
#include <random>
#include <cmath>

namespace
{
    constexpr double kSR = 48000.0;

    std::mt19937 makeRng() { return std::mt19937 (0xC0FFEE); }

    double sineResponseDb (SVFilter::Type type, double cutoff, double reso,
                           double testHz, double refHz)
    {
        auto measure = [&](double hz) -> double
        {
            SVFilter f; f.prepare (kSR); f.setType (type);
            const int warm = int (kSR * 0.2), meas = int (kSR * 0.3);
            double phase = 0.0, inc = tu::kTwoPi * hz / kSR, acc = 0.0;
            for (int i = 0; i < warm + meas; ++i)
            {
                f.setCutoff (cutoff, reso);
                const float x = float (std::sin (phase)); phase += inc;
                const float y = f.process (x);
                if (i >= warm) acc += double (y) * double (y);
            }
            return std::sqrt (acc / double (meas));
        };
        return tu::linToDb (measure (testHz) / measure (refHz));
    }
}

TEST_CASE ("filter is stable (no NaN/inf) after 10 s noise over full grid", "[filter][stability]")
{
    auto rng = makeRng();
    std::uniform_real_distribution<float> dist (-1.0f, 1.0f);

    const SVFilter::Type types[] { SVFilter::Type::LowPass, SVFilter::Type::HighPass,
                                   SVFilter::Type::BandPass, SVFilter::Type::Notch };

    for (auto type : types)
        for (double cutoff : { 20.0, 1000.0, 20000.0 })
            for (double reso : { 0.0, 0.5, 0.98 })
            {
                SVFilter f; f.prepare (kSR); f.setType (type); f.setCutoff (cutoff, reso);
                const int n = int (kSR * 10.0);
                float last = 0.0f;
                for (int i = 0; i < n; ++i)
                    last = f.process (dist (rng));
                INFO ("type=" << int (type) << " cutoff=" << cutoff << " reso=" << reso);
                REQUIRE (std::isfinite (last));
                // sanity: a stable filter driven by [-1,1] noise stays bounded
                REQUIRE (std::abs (last) < 100.0f);
            }
}

TEST_CASE ("lowpass at 1 kHz attenuates 10 kHz by > 20 dB", "[filter][response]")
{
    const double db = sineResponseDb (SVFilter::Type::LowPass, 1000.0, 0.0, 10000.0, 200.0);
    INFO ("10 kHz vs 200 Hz through 1 kHz LP: " << db << " dB");
    REQUIRE (db < -20.0);
}

TEST_CASE ("highpass at 1 kHz attenuates 100 Hz by > 20 dB (mirror)", "[filter][response]")
{
    const double db = sineResponseDb (SVFilter::Type::HighPass, 1000.0, 0.0, 100.0, 5000.0);
    INFO ("100 Hz vs 5 kHz through 1 kHz HP: " << db << " dB");
    REQUIRE (db < -20.0);
}

TEST_CASE ("full-range cutoff sweep within one block stays stable & bounded", "[filter][sweep]")
{
    auto rng = makeRng();
    std::uniform_real_distribution<float> dist (-1.0f, 1.0f);

    SVFilter f; f.prepare (kSR); f.setType (SVFilter::Type::LowPass);
    const int n = 512;
    std::vector<float> out (n);
    for (int i = 0; i < n; ++i)
    {
        // sweep cutoff 20 Hz -> 20 kHz across the block, high resonance
        const double frac = double (i) / double (n - 1);
        const double cutoff = 20.0 * std::pow (1000.0, frac);   // 20 .. 20000
        f.setCutoff (cutoff, 0.9);
        out[i] = f.process (dist (rng));
    }
    REQUIRE (tu::allFinite (out));
    REQUIRE (tu::peak (out) < 10.0f);
}

// ============================================================================
// Tier 2 — filter DRIVE (in-loop tanh saturation). drive == 0 must be the
// bit-exact legacy path; drive > 0 adds harmonics, stays bounded, and the fast
// tanh tracks std::tanh within tolerance.
// ============================================================================

TEST_CASE ("tanhFast tracks std::tanh within tolerance", "[filter][drive][tanh]")
{
    double maxErr = 0.0;
    for (double x = -8.0; x <= 8.0; x += 0.0005)
        maxErr = std::max (maxErr, std::abs (SVFilter::tanhFast (x) - std::tanh (x)));
    INFO ("max |tanhFast - std::tanh| = " << maxErr);
    REQUIRE (maxErr < 0.03);                                   // ~0.024 mid-range overshoot
    // odd, monotonic, bounded to +/-1 — the properties the in-loop saturator needs
    REQUIRE (SVFilter::tanhFast (0.0) == 0.0);
    REQUIRE (SVFilter::tanhFast (-1.7) == Catch::Approx (-SVFilter::tanhFast (1.7)).margin (1e-12));
    REQUIRE (std::abs (SVFilter::tanhFast (100.0)) <= 1.0);
    REQUIRE (SVFilter::tanhFast (0.5) < SVFilter::tanhFast (0.6));
}

TEST_CASE ("filter drive == 0 is bit-exact with the legacy linear path", "[filter][drive][golden]")
{
    SVFilter legacy;  legacy.prepare (kSR);  legacy.setType (SVFilter::Type::LowPass);
    SVFilter driven0; driven0.prepare (kSR); driven0.setType (SVFilter::Type::LowPass);
    driven0.setDrive (0.0f);                                   // must select the fast path

    auto rng = makeRng();
    std::uniform_real_distribution<float> dist (-1.0f, 1.0f);
    for (int i = 0; i < 40000; ++i)
    {
        legacy.setCutoff (1500.0, 0.7);
        driven0.setCutoff (1500.0, 0.7);
        const float x = dist (rng);
        REQUIRE (legacy.process (x) == driven0.process (x));   // exact, bit for bit
    }
}

namespace
{
    // Goertzel magnitude at `freq` over the back 3/4 of a buffer (skip the transient).
    double magAt (const std::vector<double>& y, double freq)
    {
        const double w = tu::kTwoPi * freq / kSR, c = std::cos (w);
        double s1 = 0.0, s2 = 0.0;
        for (std::size_t i = y.size() / 4; i < y.size(); ++i) { const double s = y[i] + 2.0 * c * s1 - s2; s2 = s1; s1 = s; }
        return std::sqrt (s1 * s1 + s2 * s2 - 2.0 * c * s1 * s2);
    }
}

TEST_CASE ("filter drive adds harmonics and stays bounded/finite", "[filter][drive][harmonics]")
{
    const double f1 = 220.0;
    auto renderH3overH1 = [&](float drive)
    {
        SVFilter f; f.prepare (kSR); f.setType (SVFilter::Type::LowPass); f.setDrive (drive);
        const int N = int (kSR);
        std::vector<double> y ((std::size_t) N);
        double peak = 0.0;
        for (int i = 0; i < N; ++i)
        {
            f.setCutoff (2000.0, 0.3);
            const double o = f.process (float (0.3 * std::sin (tu::kTwoPi * f1 * i / kSR)));
            y[(std::size_t) i] = o; peak = std::max (peak, std::abs (o));
        }
        REQUIRE (std::isfinite (peak));
        REQUIRE (peak < 2.0);                                  // no runaway
        return std::make_pair (magAt (y, 3 * f1) / magAt (y, f1), peak);
    };

    const auto clean  = renderH3overH1 (0.0f);
    const auto driven = renderH3overH1 (1.0f);
    INFO ("H3/H1 clean=" << clean.first << " driven=" << driven.first);
    REQUIRE (clean.first  < 1.0e-3);                           // linear path: essentially no 3rd harmonic
    REQUIRE (driven.first > 0.03);                             // driven: audible odd-harmonic content
    // level stays in a sane window (makeup keeps it within a couple dB, not blasting or dropping out)
    REQUIRE (driven.second > 0.4 * clean.second);
    REQUIRE (driven.second < 2.5 * clean.second);
}

TEST_CASE ("driven filter is stable at high resonance + full drive (noise soak)", "[filter][drive][stability]")
{
    auto rng = makeRng();
    std::uniform_real_distribution<float> dist (-1.0f, 1.0f);
    SVFilter f; f.prepare (kSR); f.setType (SVFilter::Type::LowPass); f.setDrive (1.0f);
    std::vector<float> out (200000);
    for (std::size_t i = 0; i < out.size(); ++i) { f.setCutoff (3000.0, 0.98); out[i] = f.process (dist (rng)); }
    REQUIRE (tu::allFinite (out));
    REQUIRE (tu::peak (out) < 2.0f);
}
