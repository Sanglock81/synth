// ============================================================================
// Oscillator tests — "the project's soul". A naive oscillator must fail the
// aliasing test.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PolyBlepOscillator.h"
#include "test_util.h"
#include <vector>
#include <cmath>

namespace
{
    constexpr double kSR = 48000.0;

    std::vector<float> renderOsc (PolyBlepOscillator::Wave w, double hz, int n,
                                  double pw = 0.5)
    {
        PolyBlepOscillator osc;
        osc.prepare (kSR);
        osc.setWave (w);
        osc.setPulseWidth (pw);
        osc.setFrequency (hz);
        std::vector<float> out (n);
        for (int i = 0; i < n; ++i) out[i] = osc.nextSample();
        return out;
    }

    // A deliberately naive (non-band-limited) saw, used to prove the aliasing
    // test has teeth: it must FAIL where the PolyBLEP saw passes.
    std::vector<float> renderNaiveSaw (double hz, int n)
    {
        std::vector<float> out (n);
        double phase = 0.0;
        const double inc = hz / kSR;
        for (int i = 0; i < n; ++i)
        {
            out[i] = float (2.0 * phase - 1.0);
            phase += inc;
            if (phase >= 1.0) phase -= 1.0;
        }
        return out;
    }

    // Ratio (dB) of the loudest non-harmonic peak to the fundamental.
    // f0 must NOT be an integer divisor of the sample rate, or aliased partials
    // fold exactly onto harmonics and become invisible (the 3 kHz-at-48 kHz
    // degenerate case). We use a nearby, deliberately inharmonic frequency.
    double worstAliasDb (const std::vector<float>& sig, double f0)
    {
        const std::size_t n = sig.size();
        // Blackman-Harris: residual aliasing sits far below Hann's leakage floor.
        auto mag = tu::magnitudeSpectrumWin (sig, tu::blackmanHarris);
        const double binHz = kSR / double (n);

        // Fundamental magnitude: max in a small window around f0.
        auto magAtHz = [&](double hz) -> double
        {
            const int b = int (std::lround (hz / binHz));
            double m = 0.0;
            for (int k = b - 2; k <= b + 2; ++k)
                if (k >= 0 && k < int (mag.size())) m = std::max (m, mag[std::size_t (k)]);
            return m;
        };
        const double fund = magAtHz (f0);
        REQUIRE (fund > 0.0);

        // For each bin, if it is far from every harmonic n*f0 (and from DC),
        // it is alias/noise. Track the worst.
        const double tolHz = std::max (5.0 * binHz, 0.005 * f0);   // BH main lobe
        double worst = 0.0;
        for (std::size_t b = 2; b < mag.size(); ++b)
        {
            const double hz = double (b) * binHz;
            const double nearest = std::round (hz / f0) * f0;
            if (nearest > 0.0 && std::abs (hz - nearest) <= tolHz)
                continue;                                 // on/near a harmonic
            worst = std::max (worst, mag[b]);
        }
        return tu::linToDb (worst / fund);
    }
}

TEST_CASE ("oscillator output stays bounded across 20 Hz..8 kHz, all waves", "[osc][bounds]")
{
    const PolyBlepOscillator::Wave waves[] {
        PolyBlepOscillator::Wave::Saw, PolyBlepOscillator::Wave::Square,
        PolyBlepOscillator::Wave::Triangle, PolyBlepOscillator::Wave::Sine };

    for (auto w : waves)
        for (double hz : { 20.0, 55.0, 110.0, 440.0, 1000.0, 3000.0, 8000.0 })
        {
            auto sig = renderOsc (w, hz, 1 << 14);
            INFO ("wave=" << int (w) << " hz=" << hz);
            REQUIRE (tu::allFinite (sig));
            REQUIRE (tu::peak (sig) <= 1.1f);
        }
}

TEST_CASE ("oscillator phase is continuous (no un-BLEP'd jumps)", "[osc][continuity]")
{
    // Away from the single per-cycle discontinuity, a saw's per-sample delta is
    // ~2*phaseInc. The BLEP spreads the wrap over ~2 samples, so no single delta
    // should approach the full peak-to-peak 2.0. Test sine/tri (smooth) tightly
    // and saw/square with a generous cap that a naive wrap (delta ~2.0) fails.
    SECTION ("smooth waves")
    {
        for (auto w : { PolyBlepOscillator::Wave::Sine, PolyBlepOscillator::Wave::Triangle })
            for (double hz : { 110.0, 440.0, 2000.0 })
            {
                auto sig = renderOsc (w, hz, 1 << 13);
                INFO ("wave=" << int (w) << " hz=" << hz);
                REQUIRE (tu::maxDelta (sig) < 0.5f);
            }
    }
    SECTION ("saw/square band-limited edges stay below full peak-to-peak")
    {
        for (auto w : { PolyBlepOscillator::Wave::Saw, PolyBlepOscillator::Wave::Square })
            for (double hz : { 110.0, 440.0, 2000.0 })
            {
                auto sig = renderOsc (w, hz, 1 << 13);
                INFO ("wave=" << int (w) << " hz=" << hz);
                REQUIRE (tu::maxDelta (sig) < 1.9f);   // naive wrap would be ~2.0
            }
    }
}

TEST_CASE ("PolyBLEP saw aliasing is below -60 dB (naive saw must fail)", "[osc][aliasing][soul]")
{
    // ~3 kHz but deliberately NOT a divisor of 48 kHz, so aliased partials land
    // between harmonics and are measurable. (Exactly 3 kHz at 48 kHz folds
    // aliases onto harmonics and would hide the very thing we test.)
    const double f0 = 3140.0;
    const int    n  = 1 << 15;

    const double blepDb  = worstAliasDb (renderOsc (PolyBlepOscillator::Wave::Saw, f0, n), f0);
    const double naiveDb = worstAliasDb (renderNaiveSaw (f0, n), f0);

    INFO ("polyblep worst alias = " << blepDb << " dB, naive = " << naiveDb << " dB");
    REQUIRE (blepDb  < -60.0);     // the requirement
    REQUIRE (naiveDb > -60.0);     // proves the test has teeth
}

TEST_CASE ("square PWM shifts duty cycle (0.1 / 0.5 / 0.9)", "[osc][pwm]")
{
    // Mean of a ±1 square with duty d is (d) - (1-d) = 2d - 1. Measure the DC
    // offset over whole cycles to confirm PW actually moves the second edge.
    auto meanOf = [](double pw) -> double
    {
        auto sig = renderOsc (PolyBlepOscillator::Wave::Square, 200.0, 1 << 14, pw);
        double acc = 0.0; for (float s : sig) acc += s;
        return acc / double (sig.size());
    };
    const double m10 = meanOf (0.1), m50 = meanOf (0.5), m90 = meanOf (0.9);
    INFO ("means: 0.1=" << m10 << " 0.5=" << m50 << " 0.9=" << m90);

    REQUIRE (m50 == Catch::Approx (0.0).margin (0.05));   // symmetric
    REQUIRE (m10 < m50 - 0.4);                             // ~ -0.8
    REQUIRE (m90 > m50 + 0.4);                             // ~ +0.8
    REQUIRE (m10 < -0.5);
    REQUIRE (m90 >  0.5);
}
