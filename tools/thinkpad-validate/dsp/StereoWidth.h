#pragma once
#include <algorithm>

// ============================================================================
// Stereo width (mid/side). Hand-rolled, JUCE-free, allocation-free.
//
// Splits the signal into mid = (L+R)/2 and side = (L-R)/2, scales the side by a
// width factor, and recombines. width = 0 collapses to mono, 1 is unchanged, and
// up to 2 exaggerates the stereo field. On a mono-duplicated source this is a
// no-op until an upstream effect (chorus, delay, reverb) has decorrelated the
// channels — which is exactly why it's musical to place it last (or to reorder).
//
// The width factor is smoothed per-sample so dragging the knob never clicks.
// ============================================================================

class StereoWidth
{
public:
    void prepare (double /*sampleRate*/) { reset(); }

    void reset() { smWidth = width; }

    // 0 = mono, 1 = unchanged, 2 = maximally wide.
    void setWidth (float w) { width = std::clamp (w, 0.0f, 2.0f); }

    void process (float* left, float* right, int numSamples)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            smWidth += kSmoothCoef * (width - smWidth);
            const float mid  = 0.5f * (left[i] + right[i]);
            const float side = 0.5f * (left[i] - right[i]) * smWidth;
            left[i]  = mid + side;
            right[i] = mid - side;
        }
    }

    // For the reorder crossfade: adopt another instance's smoothing state so the
    // freshly-activated chain copy continues seamlessly.
    void copyStateFrom (const StereoWidth& other) { smWidth = other.smWidth; }

private:
    static constexpr float kSmoothCoef = 0.002f;   // ~one-pole knob smoothing

    float width   = 1.0f;
    float smWidth = 1.0f;
};
