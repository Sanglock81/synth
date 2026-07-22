#pragma once
#include <algorithm>
#include <array>
#include <cmath>

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
        smSat = sat;
        for (auto& s : ap) { s.x1 = 0.0f; s.y1 = 0.0f; }
        for (auto& d : dc)  { d.x1 = 0.0f; d.y1 = 0.0f; }
    }

    // 0 = mono, 1 = unchanged, 2 = maximally wide (synthesized side at full gain).
    void setWidth (float w) { width = std::clamp (w, 0.0f, 2.0f); }

    // SAT (Tier 4c follow-on): asymmetric tube-style saturation, applied per channel BEFORE
    // the widening. 0 = clean (bit-exact bypass). The asymmetry (a small pre-bias into tanh)
    // adds EVEN harmonics — the "tube" colour the symmetric filter DRIVE doesn't give — and a
    // one-pole DC blocker removes the offset that asymmetry produces.
    void setSat (float s01) { sat = std::clamp (s01, 0.0f, 1.0f); }

    void process (float* left, float* right, int numSamples)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            smWidth += kSmoothCoef * (width - smWidth);
            smSat   += kSmoothCoef * (sat   - smSat);       // zipper-safe drive engage

            // Saturate each channel first (drive -> then widen). smSat 0 keeps the exact
            // original signal (goldens bit-identical); the crossfade makes engaging click-free.
            float l = left[i], r = right[i];
            if (smSat > 1.0e-6f)
            {
                l = saturate (l, dc[0]);
                r = saturate (r, dc[1]);
            }

            const float mid  = 0.5f * (l + r);
            const float side = 0.5f * (l - r);

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
    void copyStateFrom (const StereoWidth& other) { smWidth = other.smWidth; ap = other.ap; smSat = other.smSat; dc = other.dc; }

private:
    static constexpr float kSmoothCoef = 0.002f;   // ~one-pole knob smoothing
    static constexpr float kDecorrGain = 0.9f;     // synthesized-side level at width = 2

    // Asymmetric tube shaper. driveGain scales with smSat; the pre-bias makes the tanh
    // asymmetric (even harmonics); the result is DC-blocked, then crossfaded in by smSat so
    // engaging is click-free and smSat 0 is a true bypass.
    // Cheap tanh (clamped Padé[3/2]) — no libm call in the audio loop; saturates hard past ±3.
    static float satTanh (float x)
    {
        if (x >  3.0f) return  1.0f;
        if (x < -3.0f) return -1.0f;
        const float x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    }

    struct DCBlock { float x1 = 0.0f, y1 = 0.0f; };
    float saturate (float x, DCBlock& d) const
    {
        // Pre-gain drives the signal into the tube curve. A high max so the knob's TOP is
        // obviously distorted, and a wet mix that reaches FULL by ~8% so the rest of the
        // sweep controls DRIVE (not dry/wet) — the knob "accelerates" into saturation.
        const float driveGain = 1.0f + smSat * (kMaxSat - 1.0f);
        float sh = satTanh (driveGain * x + kBias) - kTanhBias;     // asymmetric -> even harmonics
        sh *= kMakeup;
        const float y = sh - d.x1 + kDcR * d.y1;                    // one-pole DC blocker (~4 Hz)
        d.x1 = sh; d.y1 = y;
        const float wet = std::min (smSat * kWetRamp, 1.0f);        // 0 -> full wet by smSat ~ 0.08
        return x * (1.0f - wet) + y * wet;                          // click-safe engage, then drive is the control
    }

    static constexpr float kMaxSat   = 20.0f;       // drive gain at sat = 1 (heavy tube overdrive)
    static constexpr float kWetRamp  = 12.5f;       // wet reaches 1.0 by smSat = 0.08 (fast onset)
    static constexpr float kBias     = 0.60f;       // tube asymmetry (even-harmonic amount)
    static constexpr float kTanhBias = 0.54285714f; // satTanh(0.60) — removes the static offset (x=0 -> 0)
    static constexpr float kMakeup   = 0.72f;       // level trim; the output safety clipper backstops
    static constexpr float kDcR      = 0.9995f;     // DC-blocker pole

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
    float sat     = 0.0f;
    float smSat   = 0.0f;
    std::array<DCBlock, 2> dc {};   // per-channel DC blocker state (L, R)
};
