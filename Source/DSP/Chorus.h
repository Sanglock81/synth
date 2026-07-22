#pragma once
#include <vector>
#include <cmath>
#include <algorithm>

// ============================================================================
// Stereo chorus. Hand-rolled, JUCE-free, allocation-free after prepare().
//
// A single modulated delay line per channel, the two LFOs 90° apart so a
// mono-duplicated input comes out decorrelated (wide). Delay centre ~12 ms,
// swinging ±8 ms at full depth — the classic lush chorus range. Fractional taps
// are read with linear interpolation; rate/depth/mix are smoothed so knob moves
// never zipper.
//
// VOICES (Tier 4c): with `voices == 2` a SECOND modulated tap is read per channel
// at a longer centre delay (19 ms) with its LFO inverted (180°/270°) relative to
// the first — dimension-style thickening that further decorrelates L/R. The two
// taps are summed at half weight so the wet level stays controlled. voices == 1
// (default) is the exact single-tap path (goldens bit-identical); toggling 1<->2
// crossfades via a smoothed blend (`smVoices`), so it is click-free mid-note.
//
// The delay-line buffers are sized once in prepare() (50 ms) and never resized;
// process() only reads/writes existing storage.
// ============================================================================

class Chorus
{
public:
    void prepare (double newSampleRate, int /*maxBlock*/)
    {
        sampleRate = newSampleRate;
        bufLen = (int) (newSampleRate * 0.05) + 4;      // 50 ms + guard
        bufL.assign ((size_t) bufLen, 0.0f);
        bufR.assign ((size_t) bufLen, 0.0f);
        reset();
    }

    void reset()
    {
        std::fill (bufL.begin(), bufL.end(), 0.0f);
        std::fill (bufR.begin(), bufR.end(), 0.0f);
        writePos = 0;
        phase = 0.0f;
        smMix = mix;
        smDepth = depth;
        smVoices = voices2 ? 1.0f : 0.0f;
    }

    // rateHz 0.05..8, depth 0..1, mix 0..1 (wet fraction), voices 1 or 2.
    void setParams (float rateHz, float depth01, float mix01, int voices = 1)
    {
        rate  = std::clamp (rateHz,  0.05f, 8.0f);
        depth = std::clamp (depth01, 0.0f, 1.0f);
        mix   = std::clamp (mix01,   0.0f, 1.0f);
        voices2 = voices >= 2;
    }

    void process (float* left, float* right, int numSamples)
    {
        const float phaseInc = rate / (float) sampleRate;
        const float centreSamp = kCentreMs * 0.001f * (float) sampleRate;
        const float swingSamp  = kSwingMs  * 0.001f * (float) sampleRate;

        const float centreSamp2 = kCentreMs2 * 0.001f * (float) sampleRate;
        const float voicesTarget = voices2 ? 1.0f : 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            smMix    += kSmoothCoef * (mix   - smMix);
            smDepth  += kSmoothCoef * (depth - smDepth);
            smVoices += kSmoothCoef * (voicesTarget - smVoices);   // dual-tap blend (click-safe toggle)

            bufL[(size_t) writePos] = left[i];
            bufR[(size_t) writePos] = right[i];

            const float lfoL = std::sin (kTwoPi * phase);
            const float lfoR = std::sin (kTwoPi * phase + kHalfPi);   // quadrature -> width

            const float dL = centreSamp + swingSamp * smDepth * lfoL;
            const float dR = centreSamp + swingSamp * smDepth * lfoR;

            float wetL = readInterp (bufL, (float) writePos - dL);
            float wetR = readInterp (bufR, (float) writePos - dR);

            if (smVoices > 0.0f)
            {
                // Second tap: longer centre delay, LFO at 120°/240° (independent of tap A's
                // 0°/90°) so L and R gain genuinely different modulation -> wider field. Blend
                // from tap-A-only (g=0) to half-A + half-B (g=1).
                const float lfoBL = std::sin (kTwoPi * phase + kThird);
                const float lfoBR = std::sin (kTwoPi * phase + kTwoThird);
                const float dL2 = centreSamp2 + swingSamp * smDepth * lfoBL;
                const float dR2 = centreSamp2 + swingSamp * smDepth * lfoBR;
                const float wetL2 = readInterp (bufL, (float) writePos - dL2);
                const float wetR2 = readInterp (bufR, (float) writePos - dR2);
                const float g = 0.5f * smVoices;
                wetL = wetL * (1.0f - g) + wetL2 * g;
                wetR = wetR * (1.0f - g) + wetR2 * g;
            }

            left[i]  = left[i]  * (1.0f - smMix) + wetL * smMix;
            right[i] = right[i] * (1.0f - smMix) + wetR * smMix;

            phase += phaseInc;
            if (phase >= 1.0f) phase -= 1.0f;
            if (++writePos >= bufLen) writePos = 0;
        }
    }

    void copyStateFrom (const Chorus& other)
    {
        // Buffers are identically sized (same prepare); copy in place — no alloc.
        std::copy (other.bufL.begin(), other.bufL.end(), bufL.begin());
        std::copy (other.bufR.begin(), other.bufR.end(), bufR.begin());
        writePos = other.writePos;
        phase = other.phase;
        smMix = other.smMix;
        smDepth = other.smDepth;
        smVoices = other.smVoices;
    }

private:
    // Linear-interpolated read at a fractional (possibly negative) ring position.
    float readInterp (const std::vector<float>& buf, float pos) const
    {
        while (pos < 0.0f)            pos += (float) bufLen;
        while (pos >= (float) bufLen) pos -= (float) bufLen;
        const int   i0 = (int) pos;
        const int   i1 = (i0 + 1 >= bufLen) ? 0 : i0 + 1;
        const float f  = pos - (float) i0;
        return buf[(size_t) i0] * (1.0f - f) + buf[(size_t) i1] * f;
    }

    static constexpr float kTwoPi   = 6.283185307179586f;
    static constexpr float kHalfPi  = 1.5707963267948966f;
    static constexpr float kThird    = 2.0943951023931953f;   // 120°
    static constexpr float kTwoThird = 4.1887902047863905f;   // 240°
    static constexpr float kCentreMs  = 12.0f;
    static constexpr float kCentreMs2 = 19.0f;   // second tap (Tier 4c) — a distinct, longer delay
    static constexpr float kSwingMs  = 8.0f;
    static constexpr float kSmoothCoef = 0.001f;

    double sampleRate = 44100.0;
    std::vector<float> bufL, bufR;
    int bufLen = 0;
    int writePos = 0;
    float phase = 0.0f;

    float rate = 0.8f, depth = 0.5f, mix = 0.5f;
    bool  voices2 = false;
    float smMix = 0.5f, smDepth = 0.5f, smVoices = 0.0f;
};
