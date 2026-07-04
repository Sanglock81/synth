// ============================================================================
// LFO tests: shape boundedness, S&H one-change-per-cycle, rate accuracy.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "LFO.h"
#include "test_util.h"
#include <vector>
#include <cmath>

namespace { constexpr double kSR = 48000.0; }

TEST_CASE ("all LFO shapes stay within [-1, 1]", "[lfo][bounds]")
{
    const LFO::Shape shapes[] { LFO::Shape::Triangle, LFO::Shape::Sine,
                                LFO::Shape::Square, LFO::Shape::SampleHold };
    for (auto sh : shapes)
        for (double rate : { 0.1, 2.0, 20.0 })
        {
            LFO lfo; lfo.prepare (kSR); lfo.setShape (sh); lfo.setRate (rate);
            // advance in small chunks over ~5 s
            for (int i = 0; i < int (kSR * 5.0 / 16.0); ++i)
            {
                const float v = lfo.advance (16);
                INFO ("shape=" << int (sh) << " rate=" << rate << " v=" << v);
                REQUIRE (v >= -1.0f);
                REQUIRE (v <=  1.0f);
            }
        }
}

TEST_CASE ("sample & hold changes value exactly once per cycle", "[lfo][snh]")
{
    LFO lfo; lfo.prepare (kSR); lfo.setShape (LFO::Shape::SampleHold);
    const double rate = 5.0;
    lfo.setRate (rate);

    // Advance one sample at a time for 4 s; count distinct held values.
    const int n = int (kSR * 4.0);
    int changes = 0;
    float prev = lfo.advance (1);
    for (int i = 1; i < n; ++i)
    {
        const float v = lfo.advance (1);
        if (v != prev) { ++changes; prev = v; }
    }
    const int expectedCycles = int (rate * 4.0);           // 20
    INFO ("changes=" << changes << " expected~" << expectedCycles);
    // one new value per wrap; allow +/-1 for boundary alignment
    REQUIRE (changes >= expectedCycles - 1);
    REQUIRE (changes <= expectedCycles + 1);
}

TEST_CASE ("LFO rate is accurate within 1% over 10 s", "[lfo][rate]")
{
    // Count zero-up-crossings of a sine LFO and compare to expected cycles.
    LFO lfo; lfo.prepare (kSR); lfo.setShape (LFO::Shape::Sine);
    const double rate = 3.0;
    lfo.setRate (rate);

    // Frequency from the span between the first and last up-crossing, with
    // sub-sample interpolation. This avoids the endpoint bias of a raw crossing
    // count (which misses the partial cycle at each end).
    const int n = int (kSR * 10.0);
    double firstX = -1.0, lastX = -1.0; int count = 0;
    float prev = lfo.advance (1);
    for (int i = 1; i < n; ++i)
    {
        const float v = lfo.advance (1);
        if (prev < 0.0f && v >= 0.0f)
        {
            const double frac = double (-prev) / double (v - prev);   // interp
            const double x = double (i - 1) + frac;
            if (firstX < 0.0) firstX = x; else lastX = x;
            ++count;
        }
        prev = v;
    }
    REQUIRE (count >= 3);
    const double measuredHz = double (count - 1) / ((lastX - firstX) / kSR);
    const double err = std::abs (measuredHz - rate) / rate;
    INFO ("measuredHz=" << measuredHz << " expected=" << rate << " err=" << err);
    REQUIRE (err < 0.01);
}
