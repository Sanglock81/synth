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
//
// MOTION (Tier 4a): static comb tunings ring at fixed frequencies, which reads
// metallic on a long tail. `motion` adds a very slow, very small modulation to a
// SUBSET of the combs' read positions (interpolated reads, a few samples deep,
// each modulated line on its own slow LFO so there is no single coherent wobble).
// Detuning the recirculating energy continuously smears those fixed peaks — the
// classic "chorused tail" that makes pads swim. The combs (not the allpass) are
// modulated on purpose: the combs are what set the resonant peak structure; the
// series allpass only diffuse. motion 0 (default) takes the exact original path,
// so the goldens are bit-identical.
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
        // Slow, mutually non-harmonic LFO rates (Hz) per modulated line — distinct
        // per channel so L and R never wobble together into a coherent pitch.
        constexpr double kPi = 3.14159265358979323846;
        for (int k = 0; k < kNumMod; ++k)
        {
            modEpsL[k] = 2.0f * (float) std::sin (kPi * kModRateL[k] / sampleRate);
            modEpsR[k] = 2.0f * (float) std::sin (kPi * kModRateR[k] / sampleRate);
        }
        reset();
        updateInternal();
    }

    void reset()
    {
        for (int i = 0; i < kNumCombs; ++i)   { combL[i].clear(); combR[i].clear(); }
        for (int i = 0; i < kNumAllpass; ++i) { apL[i].clear();   apR[i].clear();   }
        smMix = mix;
        smMotion = motionDepth;
        for (int k = 0; k < kNumMod; ++k) { modUL[k] = 1.0f; modVL[k] = 0.0f; modUR[k] = 1.0f; modVR[k] = 0.0f; }
    }

    // size 0..1 (room/tail), damp 0..1 (HF loss), width 0..1, mix 0..1 (wet),
    // motion 0..1 (tail modulation depth; 0 = static, bit-identical to the classic path).
    void setParams (float size01, float damp01, float width01, float mix01, float motion01 = 0.0f)
    {
        size  = std::clamp (size01, 0.0f, 1.0f);
        damp  = std::clamp (damp01, 0.0f, 1.0f);
        width = std::clamp (width01, 0.0f, 1.0f);
        mix   = std::clamp (mix01, 0.0f, 1.0f);
        motionDepth = std::clamp (motion01, 0.0f, 1.0f) * kMaxMotionSamples;   // target depth in samples
        updateInternal();
    }

    void process (float* left, float* right, int numSamples)
    {
        for (int n = 0; n < numSamples; ++n)
        {
            smMix    += kSmoothCoef * (mix - smMix);
            smMotion += kSmoothCoef * (motionDepth - smMotion);   // depth zipper-safe

            const float input = (left[n] + right[n]) * kInputGain;
            float outL = 0.0f, outR = 0.0f;

            // Per-modulated-allpass read offset (samples). The SERIES allpass diffusers are
            // modulated (not the parallel combs): wobbling the diffusion delays continuously
            // reshuffles the echo pattern and smears the fixed coloration — the classic
            // Dattorro-style de-metallizing motion. (Measurement confirmed: modulating the
            // parallel combs instead only *reinforced* the resonant peaks.) Only advance/
            // compute when motion is engaged so a static reverb stays on the original path.
            float offL[kNumAllpass] = {}, offR[kNumAllpass] = {};
            if (smMotion > 0.0f)
            {
                for (int k = 0; k < kNumMod; ++k)
                {
                    // Coupled-form ("magic circle") quadrature LFO — a stable sine with no
                    // per-sample sinf. modV ~ sin(phase), amplitude ~1.
                    modUL[k] -= modEpsL[k] * modVL[k];  modVL[k] += modEpsL[k] * modUL[k];
                    modUR[k] -= modEpsR[k] * modVR[k];  modVR[k] += modEpsR[k] * modUR[k];
                    // One-sided offset in [0, smMotion], hard-clamped for safety vs any drift.
                    offL[kModAP[k]] = std::clamp (smMotion * 0.5f * (1.0f + modVL[k]), 0.0f, kMaxMotionSamples);
                    offR[kModAP[k]] = std::clamp (smMotion * 0.5f * (1.0f + modVR[k]), 0.0f, kMaxMotionSamples);
                }
            }

            for (int i = 0; i < kNumCombs; ++i)
            {
                outL += combL[i].process (input, feedback, damping);
                outR += combR[i].process (input, feedback, damping);
            }
            for (int i = 0; i < kNumAllpass; ++i)
            {
                outL = apL[i].process (outL, offL[i]);
                outR = apR[i].process (outR, offR[i]);
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
        smMotion = other.smMotion;                                     // keep MOTION continuous across a crossfade
        for (int k = 0; k < kNumMod; ++k) { modUL[k] = other.modUL[k]; modVL[k] = other.modVL[k];
                                            modUR[k] = other.modUR[k]; modVR[k] = other.modVR[k]; }
    }

private:
    static inline float flushDenorm (float x) { return (std::abs (x) < 1.0e-15f) ? 0.0f : x; }

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

        // modOffset (samples, >=0) shortens the read delay for MOTION. modOffset == 0 is the
        // exact classic path (bufout = buf[pos]) -> goldens hold.
        float process (float input, float modOffset = 0.0f)
        {
            float bufout;
            if (modOffset > 0.0f)
            {
                const int N = (int) buf.size();
                const float rp = (float) pos + modOffset;
                int   i0   = (int) rp;
                const float frac = rp - (float) i0;
                if (i0 >= N) i0 -= N;                        // modOffset << N
                int   i1   = i0 + 1; if (i1 >= N) i1 = 0;
                bufout = buf[(size_t) i0] + frac * (buf[(size_t) i1] - buf[(size_t) i0]);
            }
            else
                bufout = buf[(size_t) pos];

            const float out = -input + bufout;
            float w = input + bufout * kApFeedback;
            if (modOffset > 0.0f) w = flushDenorm (w);
            buf[(size_t) pos] = w;
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

    // MOTION: 3 of the 4 series allpass diffusers are modulated, each on its own slow rate
    // so there is no coherent wobble; peak swing a few samples.
    static constexpr int   kNumMod = 3;
    static constexpr int   kModAP[kNumMod] { 0, 1, 2 };
    static constexpr float kMaxMotionSamples = 8.0f;                 // peak delay swing (samples) at motion=1
    static constexpr float kModRateL[kNumMod] { 0.13f, 0.19f, 0.27f };   // Hz, non-harmonic
    static constexpr float kModRateR[kNumMod] { 0.15f, 0.22f, 0.29f };

    // Classic Freeverb tunings (samples @ 44.1 kHz).
    static constexpr int kCombTuning[kNumCombs]     { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
    static constexpr int kAllpassTuning[kNumAllpass] { 556, 441, 341, 225 };

    double sampleRate = 44100.0;
    Comb    combL[kNumCombs],   combR[kNumCombs];
    Allpass apL[kNumAllpass],   apR[kNumAllpass];

    float size = 0.5f, damp = 0.5f, width = 1.0f, mix = 0.3f;
    float feedback = 0.84f, damping = 0.2f, smMix = 0.3f;

    float motionDepth = 0.0f, smMotion = 0.0f;                      // target/smoothed depth (samples)
    float modEpsL[kNumMod] {}, modEpsR[kNumMod] {};                 // per-line LFO tuning
    float modUL[kNumMod] { 1,1,1 }, modVL[kNumMod] {};              // quadrature state (u~cos, v~sin)
    float modUR[kNumMod] { 1,1,1 }, modVR[kNumMod] {};
};
