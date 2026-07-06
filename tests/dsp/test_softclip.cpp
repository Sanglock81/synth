// ============================================================================
// Output safety clipper (Bug 4). The clipper is the last stage before the DAC;
// its whole job is to (a) never let the output exceed +/-1.0 and (b) be a
// BIT-EXACT passthrough below its threshold so normal playing is untouched.
// JUCE-free.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "SoftClip.h"
#include <cmath>
#include <limits>

TEST_CASE ("SoftClip: bit-exact passthrough below threshold", "[bug4][softclip]")
{
    const float t = SoftClip::kThreshold;
    // Sweep the whole passthrough band, including the endpoints, at fine steps.
    for (int i = -20000; i <= 20000; ++i)
    {
        const float x = (float) i / 20000.0f * t;      // |x| in [0, t]
        bool engaged = false;
        const float y = SoftClip::process (x, engaged);
        REQUIRE (y == x);                                // exact, bit for bit
        REQUIRE_FALSE (engaged);                         // knee never used below t
    }
    // Exactly at the threshold is still passthrough (|x| <= t).
    bool e1 = false, e2 = false;
    REQUIRE (SoftClip::process (t,  e1) == t);
    REQUIRE (SoftClip::process (-t, e2) == -t);
    REQUIRE_FALSE (e1); REQUIRE_FALSE (e2);
}

TEST_CASE ("SoftClip: output bounded in [-1,1] for any input", "[bug4][softclip]")
{
    // The knee asymptotes to +/-1: it never EXCEEDS full scale (the invariant),
    // though for extreme inputs tanh saturates to exactly 1.0f in float — a valid
    // full-scale sample, not an over.
    for (float x : { 0.81f, 0.9f, 1.0f, 2.0f, 10.0f, 1.0e6f,
                     -0.81f, -0.9f, -1.0f, -2.0f, -10.0f, -1.0e6f })
    {
        const float y = SoftClip::process (x);
        REQUIRE (std::abs (y) <= 1.0f);
        REQUIRE (std::isfinite (y));
    }
    REQUIRE (std::abs (SoftClip::process (std::numeric_limits<float>::max())) <= 1.0f);
    // Moderate overs stay strictly inside FS (only pathological inputs hit 1.0).
    REQUIRE (SoftClip::process (2.0f) < 1.0f);
}

TEST_CASE ("SoftClip: engages only above threshold, sign-symmetric, monotonic", "[bug4][softclip]")
{
    const float t = SoftClip::kThreshold;

    bool below = false, above = false;
    SoftClip::process (t - 0.01f, below);
    SoftClip::process (t + 0.01f, above);
    REQUIRE_FALSE (below);
    REQUIRE (above);

    // Odd symmetry: process(-x) == -process(x).
    for (float x : { 0.5f, 0.85f, 1.5f, 5.0f })
        REQUIRE (SoftClip::process (-x) == -SoftClip::process (x));

    // Monotonic non-decreasing across the knee.
    float prev = -2.0f;
    for (int i = 0; i <= 4000; ++i)
    {
        const float x = -2.0f + (float) i / 1000.0f;     // -2 .. +2
        const float y = SoftClip::process (x);
        REQUIRE (y >= prev - 1.0e-7f);
        prev = y;
    }
}

TEST_CASE ("SoftClip: slope-continuous at the joint (no kink)", "[bug4][softclip]")
{
    const float t = SoftClip::kThreshold;
    const float h = 1.0e-4f;
    // One-sided slopes either side of the threshold must match ~1 (the y=x slope).
    const float slopeBelow = (SoftClip::process (t) - SoftClip::process (t - h)) / h;
    const float slopeAbove = (SoftClip::process (t + h) - SoftClip::process (t)) / h;
    REQUIRE (slopeBelow == Catch::Approx (1.0f).margin (1.0e-3));
    REQUIRE (slopeAbove == Catch::Approx (1.0f).margin (1.0e-2));   // knee starts at unit slope
}
