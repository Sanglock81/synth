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
    void prepare (double sampleRate) { sr = (sampleRate > 0.0) ? (float) sampleRate : 48000.0f; designHalfband(); reset(); }

    void reset()
    {
        smWidth = width;
        smSat = sat;
        for (auto& s : ap)    { s.x1 = 0.0f; s.y1 = 0.0f; }
        for (auto& s : satCh) s = SatChannel {};
    }

    // 0 = mono, 1 = unchanged, 2 = maximally wide (synthesized side at full gain).
    void setWidth (float w) { width = std::clamp (w, 0.0f, 2.0f); }

    // SAT: a TWO-STAGE variable-threshold clipper, applied per channel BEFORE the widening. It is
    // NOT a pre-gain-into-a-ceiling (that just makes everything loud) — the shaper has UNITY slope
    // near zero, so a signal below the threshold passes ~unchanged and only what pokes above it
    // clips. The knob lowers the threshold across the WHOLE range (progressively more clipping) but
    // the KNEE and a smoothing filter split the sweep into two musically distinct halves:
    //   sat 0        -> threshold high -> nothing clips (a true bit-exact bypass; goldens hold).
    //   sat 0..0.5   -> SOFT clip, driven progressively harder, then ROUNDED by a lowpass (warm,
    //                   smooth overdrive). Velocity-sensitive: quiet notes stay clean, loud clip.
    //   sat 0.5..1.0 -> the knee HARDENS (soft tanh -> hard clamp) and the smoothing lowpass OPENS
    //                   up, so the clip gets rawer and edgier toward a hard square (fuzz) at the top.
    // A small asymmetric bias adds EVEN harmonics (tube warmth); a DC blocker removes the offset.
    // Loudness is kept ~constant across the sweep (reference-RMS-flat makeup, peak-guarded so it is
    // never a hidden boost) — the per-part LEVEL at the end of the chain stays the volume. Runs
    // 2x-oversampled while engaged (hard clipping aliases); the wet crossfade folds the rate-domain
    // handoff in/out click-free as sat crosses zero.
    void setSat (float s01) { sat = std::clamp (s01, 0.0f, 1.0f); }

    void process (float* left, float* right, int numSamples)
    {
        // Threshold + knee + smoothing follow the KNOB target (constant per block): the clipping
        // tracks the knob immediately, while the wet crossfade below handles click-safe engage.
        //   top = the upper-half progress: 0 through sat<=0.5, then 0->1 over 0.5..0.9 (so the knee is
        //         a full hard clamp — a FUZZ — by 90 %, holding there to 100 %). Drives BOTH the knee
        //         hardening AND the smoothing-lowpass opening, so the two move together.
        //   T   = a two-segment exponential threshold: kThi (no clip) -> kTmid at the midpoint (a
        //         solid soft overdrive) -> kTlo by 90 % (hard square). Monotonic and tuned so a single
        //         note starts to break up by ~30 %. pow() twice per block is cheap.
        const float top = std::clamp ((sat - 0.5f) / 0.4f, 0.0f, 1.0f);
        const float T   = (sat <= 0.5f) ? kThi  * std::pow (kTmid / kThi, sat * 2.0f)
                                        : kTmid * std::pow (kTlo  / kTmid, top);
        const float h   = top;                                     // soft (tanh) below mid, hard by 90 %
        // Smoothing lowpass: low cutoff (rounds the soft clip -> warm) in the bottom half, opening
        // toward Nyquist (raw, edgy hard clip) in the top half. One-pole coefficient, per block.
        const float fc  = kLpMin + (kLpMax - kLpMin) * top;
        const float lpA = 1.0f - std::exp (-kTwoPi * fc / sr);

        for (int i = 0; i < numSamples; ++i)
        {
            smWidth += kSmoothCoef * (width - smWidth);
            smSat   += kSmoothCoef * (sat   - smSat);       // zipper-safe engage

            // Clip each channel first (clip -> then widen). smSat 0 keeps the exact original
            // signal (goldens bit-identical); the wet crossfade makes engaging click-free.
            float l = left[i], r = right[i];
            if (smSat > 1.0e-6f)
            {
                const float wet = std::min (smSat * kWetRamp, 1.0f); // 0 -> full by smSat ~ 0.08
                l = saturate (satCh[0], l, T, h, lpA, wet);
                r = saturate (satCh[1], r, T, h, lpA, wet);
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
    void copyStateFrom (const StereoWidth& other) { smWidth = other.smWidth; ap = other.ap; smSat = other.smSat; satCh = other.satCh; }

private:
    static constexpr float kSmoothCoef = 0.002f;   // ~one-pole knob smoothing
    static constexpr float kDecorrGain = 0.9f;     // synthesized-side level at width = 2

    static constexpr int   kHbLen       = 11;      // FIR half-band length (matches the filter)
    static constexpr int   kHbCenterIdx = 5;
    static constexpr int   kHbNumOdd    = 3;       // non-zero odd-offset taps: 1, 3, 5

    struct SatChannel                              // per-channel 2x oversampler + DC + level state
    {
        float upPrev = 0.0f;
        std::array<float, kHbLen> downZ {};
        int   downPos = 0;
        float dcx1 = 0.0f, dcy1 = 0.0f;
        float inEnv = 0.0f, outEnv = 0.0f;         // slow |amp| followers for the auto-makeup
        float lp = 0.0f;                           // one-pole smoothing lowpass state (rounds the clip)
    };

    // Cheap tanh (clamped Padé[3/2]) — the SOFT clipper shape; unity slope at 0, saturates ±1.
    static float satTanh (float x)
    {
        if (x >  3.0f) return  1.0f;
        if (x < -3.0f) return -1.0f;
        const float x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    }

    // The SOFT clipper shape, ASYMMETRIC (tube-like): both polarities have UNITY slope at zero, so a
    // sub-threshold signal passes clean (velocity sensitivity preserved, no distortion floor), but
    // the positive side saturates sooner than the negative (kAsymK < 1 pushes the negative knee out).
    // The two sides therefore diverge only as the signal is DRIVEN past the knee — that asymmetry is
    // what generates the EVEN harmonics (warmth) and a drive-dependent DC the blocker removes.
    static float softShape (float n)
    {
        return (n >= 0.0f) ? satTanh (n) : satTanh (n * kAsymK) * (1.0f / kAsymK);
    }

    // Threshold clipper at ONE (over)sample. `T` is the clip threshold, `h` the knee hardness
    // (0 = asymmetric soft, 1 = hard clamp). Normalise to the threshold, blend soft->hard, rescale
    // by T: small |x| -> ~x (unity, clean); large |x| -> clipped (asymmetric warmth, then square).
    static float clipCore (float x, float T, float h)
    {
        const float n    = x / T;
        const float soft = softShape (n);
        const float hard = std::clamp (n, -1.0f, 1.0f);
        return T * ((1.0f - h) * soft + h * hard);
    }

    // Per-channel 2x-oversampled clip: linear-interp upsample (the input is already band-limited),
    // clip both sub-samples, FIR half-band decimate, then an ENVELOPE-following makeup + DC block
    // at base rate, then the wet crossfade (click-safe engage as sat crosses zero).
    //
    // The makeup restores each note to its OWN input loudness (inEnv/outEnv, slow followers): the
    // clip removes level, the makeup puts it back, so loudness stays flat across the SAT sweep AND
    // across velocity — the part LEVEL stays the volume control. Because clipping lowers the crest
    // factor, restoring the RMS can never push the peak above the input peak, so it is never a
    // hidden boost. Clamped to [1, kMaxMakeup] (only ever restores, never attenuates or runs away).
    float saturate (SatChannel& s, float x, float T, float h, float lpA, float wet) const
    {
        const float u1 = x;
        const float u0 = 0.5f * (s.upPrev + u1);
        s.upPrev = u1;
        pushDown (s, clipCore (u0, T, h));
        const float dec = pushDown (s, clipCore (u1, T, h));

        // Smoothing lowpass on the clipped signal: rounds the harsh clip edges (warm, soft) at low
        // SAT; the coefficient opens toward passthrough at high SAT so the hard clip stays raw.
        s.lp += lpA * (dec - s.lp);
        const float sm = s.lp;

        // Makeup measured on the SMOOTHED signal, so it restores the level lost to BOTH the clip and
        // the lowpass (keeping SAT loudness-neutral even while it darkens the tone).
        s.inEnv  += kEnvCoef * (std::abs (x)  - s.inEnv);
        s.outEnv += kEnvCoef * (std::abs (sm) - s.outEnv);
        const float makeup = (s.outEnv > 1.0e-5f) ? std::clamp (s.inEnv / s.outEnv, 1.0f, kMaxMakeup) : 1.0f;

        const float m = sm * makeup;
        const float y = m - s.dcx1 + kDcR * s.dcy1;        // one-pole DC blocker (~4 Hz)
        s.dcx1 = m; s.dcy1 = y;
        return x * (1.0f - wet) + y * wet;
    }

    // FIR half-band decimator (mirrors the filter's 2C oversampler): push one 2x sample, return
    // the filtered value. Even-offset taps (bar the centre) are zero, so only centre + odd taps
    // cost a multiply. The caller keeps the value returned on the SECOND push (the decimated one).
    float pushDown (SatChannel& s, float x) const
    {
        s.downZ[(std::size_t) s.downPos] = x;
        float acc = hbCenter * s.downZ[(std::size_t) ((s.downPos - kHbCenterIdx + kHbLen) % kHbLen)];
        for (int j = 0; j < kHbNumOdd; ++j)
        {
            const int off = 2 * j + 1;
            const int ia = (s.downPos - (kHbCenterIdx - off) + kHbLen) % kHbLen;
            const int ib = (s.downPos - (kHbCenterIdx + off) + kHbLen) % kHbLen;
            acc += hbOdd[(std::size_t) j] * (s.downZ[(std::size_t) ia] + s.downZ[(std::size_t) ib]);
        }
        s.downPos = (s.downPos + 1) % kHbLen;
        return acc;
    }

    void designHalfband()
    {
        constexpr double pi = 3.14159265358979323846;
        double h[kHbLen]; double sum = 0.0;
        for (int n = 0; n < kHbLen; ++n)
        {
            const int m = n - kHbCenterIdx;
            const double sinc = (m == 0) ? 0.5 : std::sin (0.5 * pi * m) / (pi * m);   // fc = 0.25 (halfband)
            const double w    = 0.54 - 0.46 * std::cos (2.0 * pi * n / (kHbLen - 1));  // Hamming
            h[n] = sinc * w; sum += h[n];
        }
        for (int n = 0; n < kHbLen; ++n) h[n] /= sum;                                   // unity DC gain
        hbCenter = (float) h[kHbCenterIdx];
        for (int j = 0; j < kHbNumOdd; ++j) hbOdd[(std::size_t) j] = (float) h[kHbCenterIdx + (2 * j + 1)];
    }

    static constexpr float kTwoPi     = 6.28318530717958647692f;
    static constexpr float kWetRamp   = 12.5f;     // wet reaches 1.0 by smSat = 0.08 (fast onset)
    static constexpr float kThi       = 0.45f;     // threshold at sat -> 0 (single notes break up by ~30 %)
    static constexpr float kTmid      = 0.06f;     // threshold at sat = 0.5 (well into soft overdrive)
    static constexpr float kTlo       = 0.015f;    // threshold reached by 90 % (hard square -> fuzz)
    static constexpr float kAsymK     = 0.42f;     // soft-clip asymmetry: negative knee pushed out 1/k -> even harmonics
    static constexpr float kLpMin     = 5500.0f;   // smoothing-lowpass cutoff at sat <= 0.5 (rounds the soft clip)
    static constexpr float kLpMax     = 19000.0f;  // ...opening to ~passthrough at sat = 1 (raw hard clip)
    static constexpr float kDcR       = 0.9995f;   // DC-blocker pole
    static constexpr float kEnvCoef   = 0.0007f;   // ~30 ms |amp| follower for the auto-makeup
    static constexpr float kMaxMakeup = 24.0f;     // makeup ceiling — high enough to restore level after the aggressive fuzz clip

    std::array<SatChannel, 2> satCh {};            // per-channel oversampler + DC state (L, R)
    float hbCenter = 0.5f;
    std::array<float, kHbNumOdd> hbOdd {};

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

    float sr      = 48000.0f;   // base sample rate (for the smoothing-lowpass coefficient)
    float width   = 1.0f;
    float smWidth = 1.0f;
    float sat     = 0.0f;
    float smSat   = 0.0f;
};
