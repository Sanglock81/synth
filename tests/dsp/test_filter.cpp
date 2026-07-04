// ============================================================================
// Filter tests: stability under sustained noise across the full parameter grid,
// frequency-response sanity, and behaviour under a full-range cutoff sweep.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
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
