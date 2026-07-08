#pragma once
#include <vector>
#include <cmath>
#include <algorithm>

// ============================================================================
// Stereo delay with cross-feedback (ping-pong). Hand-rolled, JUCE-free,
// allocation-free after prepare().
//
// Each channel has its own delay line. Feedback is routed L->R and R->L so
// repeats bounce across the field (ping-pong) while a `spread` of 0 collapses to
// a plain mono-style stereo delay (feedback stays on its own side). A one-pole
// low-pass in the feedback path makes successive repeats darker, like tape.
//
// Delay time is smoothed toward its target so turning the knob glides instead of
// clicking; buffers are sized for the max time once in prepare() (1.5 s).
// ============================================================================

class StereoDelay
{
public:
    void prepare (double newSampleRate, int /*maxBlock*/)
    {
        sampleRate = newSampleRate;
        bufLen = (int) (newSampleRate * kMaxSeconds) + 4;
        bufL.assign ((size_t) bufLen, 0.0f);
        bufR.assign ((size_t) bufLen, 0.0f);
        reset();
    }

    void reset()
    {
        std::fill (bufL.begin(), bufL.end(), 0.0f);
        std::fill (bufR.begin(), bufR.end(), 0.0f);
        writePos = 0;
        lpL = lpR = 0.0f;
        smMix = mix;
        smDelay = timeSamp();
    }

    // timeMs 1..1500, feedback 0..~0.95, mix 0..1, spread 0..1 (ping-pong amount).
    void setParams (float timeMs, float feedback01, float mix01, float spread01)
    {
        timeMs   = std::clamp (timeMs, 1.0f, kMaxSeconds * 1000.0f - 1.0f);
        delaySamp = timeMs * 0.001f * (float) sampleRate;
        feedback = std::clamp (feedback01, 0.0f, 0.95f);
        mix      = std::clamp (mix01, 0.0f, 1.0f);
        spread   = std::clamp (spread01, 0.0f, 1.0f);
    }

    void process (float* left, float* right, int numSamples)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            smMix   += kSmoothCoef * (mix - smMix);
            smDelay += kSmoothCoef * (delaySamp - smDelay);

            const float wetL = readInterp (bufL, (float) writePos - smDelay);
            const float wetR = readInterp (bufR, (float) writePos - smDelay);

            // Feedback: crossed by `spread` (ping-pong), self by the remainder,
            // darkened by a one-pole LP so repeats decay in brightness too.
            const float fbFromL = left[i]  + feedback * wetL;
            const float fbFromR = right[i] + feedback * wetR;
            const float inL = (1.0f - spread) * fbFromL + spread * fbFromR;
            const float inR = (1.0f - spread) * fbFromR + spread * fbFromL;

            lpL += kDamp * (inL - lpL);
            lpR += kDamp * (inR - lpR);
            bufL[(size_t) writePos] = lpL;
            bufR[(size_t) writePos] = lpR;

            left[i]  = left[i]  * (1.0f - smMix) + wetL * smMix;
            right[i] = right[i] * (1.0f - smMix) + wetR * smMix;

            if (++writePos >= bufLen) writePos = 0;
        }
    }

    void copyStateFrom (const StereoDelay& other)
    {
        std::copy (other.bufL.begin(), other.bufL.end(), bufL.begin());
        std::copy (other.bufR.begin(), other.bufR.end(), bufR.begin());
        writePos = other.writePos;
        lpL = other.lpL; lpR = other.lpR;
        smMix = other.smMix; smDelay = other.smDelay;
    }

private:
    float timeSamp() const { return delaySamp; }

    float readInterp (const std::vector<float>& buf, float pos) const
    {
        while (pos < 0.0f)            pos += (float) bufLen;
        while (pos >= (float) bufLen) pos -= (float) bufLen;
        const int   i0 = (int) pos;
        const int   i1 = (i0 + 1 >= bufLen) ? 0 : i0 + 1;
        const float f  = pos - (float) i0;
        return buf[(size_t) i0] * (1.0f - f) + buf[(size_t) i1] * f;
    }

    static constexpr float kMaxSeconds = 1.5f;
    static constexpr float kSmoothCoef = 0.0005f;   // slow time glide (no pitch zip)
    static constexpr float kDamp = 0.35f;           // feedback-path LP amount

    double sampleRate = 44100.0;
    std::vector<float> bufL, bufR;
    int bufLen = 0;
    int writePos = 0;
    float lpL = 0.0f, lpR = 0.0f;

    float delaySamp = 4800.0f;   // 100 ms @ 48k default
    float feedback = 0.35f, mix = 0.35f, spread = 1.0f;
    float smMix = 0.35f, smDelay = 4800.0f;
};
