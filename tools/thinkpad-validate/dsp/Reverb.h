#pragma once
#include <vector>
#include <cmath>
#include <algorithm>

// ============================================================================
// Reverb — hand-rolled Schroeder/Moorer "Freeverb" topology (after Jezar's
// public-domain design). JUCE-free, allocation-free after prepare().
//
// Per channel: 8 parallel damped comb filters (the dense tail) feeding 4 series
// allpass filters (diffusion). The right channel's tunings are offset by a small
// "stereo spread" so the two channels decorrelate into a wide field. Comb tunings
// are the classic Freeverb primes, scaled from their 44.1 kHz originals to the
// actual sample rate so the room tunes correctly at 48 kHz.
//
// Params: size (tail length via comb feedback), damp (HF absorption), width
// (stereo spread of the wet), mix (dry/wet). All buffers are allocated once in
// prepare(); process() only touches existing storage.
// ============================================================================

class Reverb
{
public:
    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate;
        const double scale = newSampleRate / 44100.0;

        for (int i = 0; i < kNumCombs; ++i)
        {
            combL[i].resize (scaled (kCombTuning[i], scale));
            combR[i].resize (scaled (kCombTuning[i] + kStereoSpread, scale));
        }
        for (int i = 0; i < kNumAllpass; ++i)
        {
            apL[i].resize (scaled (kAllpassTuning[i], scale));
            apR[i].resize (scaled (kAllpassTuning[i] + kStereoSpread, scale));
        }
        reset();
        updateInternal();
    }

    void reset()
    {
        for (int i = 0; i < kNumCombs; ++i)   { combL[i].clear(); combR[i].clear(); }
        for (int i = 0; i < kNumAllpass; ++i) { apL[i].clear();   apR[i].clear();   }
        smMix = mix;
    }

    // size 0..1 (room/tail), damp 0..1 (HF loss), width 0..1, mix 0..1 (wet).
    void setParams (float size01, float damp01, float width01, float mix01)
    {
        size  = std::clamp (size01, 0.0f, 1.0f);
        damp  = std::clamp (damp01, 0.0f, 1.0f);
        width = std::clamp (width01, 0.0f, 1.0f);
        mix   = std::clamp (mix01, 0.0f, 1.0f);
        updateInternal();
    }

    void process (float* left, float* right, int numSamples)
    {
        for (int n = 0; n < numSamples; ++n)
        {
            smMix += kSmoothCoef * (mix - smMix);

            const float input = (left[n] + right[n]) * kInputGain;
            float outL = 0.0f, outR = 0.0f;

            for (int i = 0; i < kNumCombs; ++i)
            {
                outL += combL[i].process (input, feedback, damping);
                outR += combR[i].process (input, feedback, damping);
            }
            for (int i = 0; i < kNumAllpass; ++i)
            {
                outL = apL[i].process (outL);
                outR = apR[i].process (outR);
            }

            // Width blends the two wet channels toward mono (width 0) or keeps
            // them fully separated (width 1).
            const float wet1 = 0.5f * (1.0f + width);
            const float wet2 = 0.5f * (1.0f - width);
            const float wL = outL * wet1 + outR * wet2;
            const float wR = outR * wet1 + outL * wet2;

            left[n]  = left[n]  * (1.0f - smMix) + wL * smMix;
            right[n] = right[n] * (1.0f - smMix) + wR * smMix;
        }
    }

    void copyStateFrom (const Reverb& other)
    {
        for (int i = 0; i < kNumCombs; ++i)   { combL[i].copyStateFrom (other.combL[i]); combR[i].copyStateFrom (other.combR[i]); }
        for (int i = 0; i < kNumAllpass; ++i) { apL[i].copyStateFrom (other.apL[i]);     apR[i].copyStateFrom (other.apR[i]); }
        smMix = other.smMix;
    }

private:
    // ---- comb filter with a one-pole damping LP in the feedback path ----------
    struct Comb
    {
        std::vector<float> buf;
        int pos = 0;
        float store = 0.0f;

        void resize (int n) { buf.assign ((size_t) std::max (1, n), 0.0f); }
        void clear() { std::fill (buf.begin(), buf.end(), 0.0f); pos = 0; store = 0.0f; }

        float process (float input, float feedback, float damp)
        {
            const float out = buf[(size_t) pos];
            store = out * (1.0f - damp) + store * damp;         // LP the feedback
            buf[(size_t) pos] = input + store * feedback;
            if (++pos >= (int) buf.size()) pos = 0;
            return out;
        }
        void copyStateFrom (const Comb& o)
        {
            std::copy (o.buf.begin(), o.buf.end(), buf.begin());
            pos = o.pos; store = o.store;
        }
    };

    // ---- Schroeder allpass ----------------------------------------------------
    struct Allpass
    {
        std::vector<float> buf;
        int pos = 0;

        void resize (int n) { buf.assign ((size_t) std::max (1, n), 0.0f); }
        void clear() { std::fill (buf.begin(), buf.end(), 0.0f); pos = 0; }

        float process (float input)
        {
            const float bufout = buf[(size_t) pos];
            const float out = -input + bufout;
            buf[(size_t) pos] = input + bufout * kApFeedback;
            if (++pos >= (int) buf.size()) pos = 0;
            return out;
        }
        void copyStateFrom (const Allpass& o)
        {
            std::copy (o.buf.begin(), o.buf.end(), buf.begin());
            pos = o.pos;
        }
    };

    void updateInternal()
    {
        // Freeverb mapping: size -> comb feedback, damp -> LP coefficient.
        feedback = size * (kFeedbackMax - kFeedbackMin) + kFeedbackMin;
        damping  = damp * kDampScale;
    }

    static int scaled (int tuning44k, double scale) { return std::max (1, (int) std::lround (tuning44k * scale)); }

    static constexpr int kNumCombs   = 8;
    static constexpr int kNumAllpass = 4;
    static constexpr int kStereoSpread = 23;      // right-channel tuning offset (samples @44.1k)
    static constexpr float kApFeedback = 0.5f;
    static constexpr float kInputGain  = 0.015f;  // keeps the dense tail from clipping
    static constexpr float kFeedbackMin = 0.7f;
    static constexpr float kFeedbackMax = 0.98f;
    static constexpr float kDampScale   = 0.4f;
    static constexpr float kSmoothCoef  = 0.002f;

    // Classic Freeverb tunings (samples @ 44.1 kHz).
    static constexpr int kCombTuning[kNumCombs]     { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
    static constexpr int kAllpassTuning[kNumAllpass] { 556, 441, 341, 225 };

    double sampleRate = 44100.0;
    Comb    combL[kNumCombs],   combR[kNumCombs];
    Allpass apL[kNumAllpass],   apR[kNumAllpass];

    float size = 0.5f, damp = 0.5f, width = 1.0f, mix = 0.3f;
    float feedback = 0.84f, damping = 0.2f, smMix = 0.3f;
};
