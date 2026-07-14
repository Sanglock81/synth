#pragma once
#include <algorithm>
#include <array>

// ============================================================================
// Stereo width (mid/side) with mono-safe widening. Hand-rolled, JUCE-free,
// allocation-free.
//
//   width <= 1.0 : pure mid/side scaling. mid = (L+R)/2, side = (L-R)/2, side
//                  scaled by width. 0 collapses to mono, 1 is unchanged. This is
//                  narrowing / passthrough — bit-exact at width == 1.
//   width  > 1.0 : the existing side is kept at unity AND extra side content is
//                  SYNTHESIZED from the mid signal through an allpass cascade,
//                  scaled by (width - 1). This is what makes a DRY MONO patch
//                  audibly widen — a plain mid/side scaler can't, because a mono
//                  source has no side signal to scale.
//
// Why an allpass cascade and NOT a Haas delay: a Haas (fixed short delay) creates
// comb-filter notches when the stereo image is folded back to mono. An allpass
// cascade decorrelates by PHASE only — its magnitude response is flat — so the
// synthesized content colours nothing. And because it is added purely to the SIDE
// (antisymmetrically: L += d, R -= d), it CANCELS on a mono sum (L+R = 2*mid):
// the mono fold-down is identical to the un-widened mono sum, no ripple.
//
// The width factor is smoothed per-sample so dragging the knob never clicks; the
// allpass state runs continuously (fed the mid every sample) so its contribution
// fades in from silence with no transient when width crosses 1.0.
// ============================================================================

class StereoWidth
{
public:
    void prepare (double /*sampleRate*/) { reset(); }

    void reset()
    {
        smWidth = width;
        for (auto& s : ap) { s.x1 = 0.0f; s.y1 = 0.0f; }
    }

    // 0 = mono, 1 = unchanged, 2 = maximally wide (synthesized side at full gain).
    void setWidth (float w) { width = std::clamp (w, 0.0f, 2.0f); }

    void process (float* left, float* right, int numSamples)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            smWidth += kSmoothCoef * (width - smWidth);
            const float mid  = 0.5f * (left[i] + right[i]);
            const float side = 0.5f * (left[i] - right[i]);

            // Decorrelate the mid through the allpass cascade (state runs always).
            float decorr = mid;
            for (auto& s : ap) decorr = s.process (decorr);

            float outSide;
            if (smWidth <= 1.0f)
                outSide = side * smWidth;                                  // narrow / unity
            else
                outSide = side + (smWidth - 1.0f) * kDecorrGain * decorr;  // add synthesized side

            left[i]  = mid + outSide;
            right[i] = mid - outSide;
        }
    }

    // For the reorder crossfade: adopt another instance's smoothing + filter state
    // so the freshly-activated chain copy continues seamlessly.
    void copyStateFrom (const StereoWidth& other) { smWidth = other.smWidth; ap = other.ap; }

private:
    static constexpr float kSmoothCoef = 0.002f;   // ~one-pole knob smoothing
    static constexpr float kDecorrGain = 0.9f;     // synthesized-side level at width = 2

    // First-order Schroeder allpass: H(z) = (a + z^-1) / (1 + a z^-1).
    struct Allpass
    {
        float a = 0.0f, x1 = 0.0f, y1 = 0.0f;
        float process (float x)
        {
            const float y = a * x + x1 - a * y1;
            x1 = x; y1 = y;
            return y;
        }
    };

    // Fixed, well-spread coefficients (alternating sign) — a deterministic phase
    // scramble across the spectrum. Not sample-rate critical (phase-only).
    std::array<Allpass, 5> ap { { { 0.70f }, { -0.62f }, { 0.53f }, { -0.44f }, { 0.35f } } };

    float width   = 1.0f;
    float smWidth = 1.0f;
};
