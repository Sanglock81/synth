#pragma once
#include <cmath>

// ============================================================================
// Output safety clipper. Hand-rolled, JUCE-free, stateless.
//
// A summing polysynth can drive its output well past +/-1.0 on dense chords
// (16 voices at unity = several times full-scale). Hard digital clipping at the
// DAC then sounds like crackle/pops. This is the final safety stage that keeps
// the signal bounded WITHOUT coloring normal-level playing.
//
// The curve is a SPLICED soft clip, transparent below the threshold:
//
//     y = x                                        for |x| <= t   (bit-exact)
//     y = t + (1-t) * tanh((|x|-t) / (1-t))        for |x| >  t   (sign-mirrored)
//
// It is C1-continuous at the joint: the tanh knee has unit slope at over=0, so
// it meets the y=x line at slope 1 (no kink). It asymptotes to +/-1, so the
// output never EXCEEDS full scale (bounded in [-1, 1] — only a pathological
// input drives tanh to saturate to exactly 1.0f in float). Crucially, below
// t it is an EXACT passthrough (y == x, bit for bit) — quiet/solo material is
// untouched. (A normalized tanh(kx)/tanh(k) was rejected: its small-signal slope
// k/tanh(k) > 1 boosts and colors everything.)
//
// NOTE (aliasing): this is a static nonlinearity, so when heavily engaged it can
// generate harmonics above Nyquist that fold back. That is acceptable for a
// rarely-touched safety stage (normal playing stays below t and never engages).
// Oversampling the clipper is a future HQ-mode option, not done here.
// ============================================================================

struct SoftClip
{
    static constexpr float kThreshold = 0.8f;   // t: passthrough below, knee above

    // Clip one sample. Sets `engaged` true iff the knee (nonlinear region) was
    // used — used for the saturation-activity telemetry. `engaged` is only ever
    // set to true (never cleared) so callers can OR it across a block.
    static inline float process (float x, bool& engaged, float t = kThreshold) noexcept
    {
        const float a = std::fabs (x);
        if (a <= t)
            return x;                                  // exact passthrough
        engaged = true;
        const float knee = t + (1.0f - t) * std::tanh ((a - t) / (1.0f - t));
        return x < 0.0f ? -knee : knee;
    }

    // Convenience overload when engagement isn't needed.
    static inline float process (float x, float t = kThreshold) noexcept
    {
        bool ignore = false;
        return process (x, ignore, t);
    }
};
