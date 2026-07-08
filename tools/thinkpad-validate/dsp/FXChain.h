#pragma once
#include "Chorus.h"
#include "StereoDelay.h"
#include "Reverb.h"
#include "StereoWidth.h"
#include <vector>
#include <cmath>
#include <algorithm>

// ============================================================================
// Global reorderable stereo FX chain. Hand-rolled, JUCE-free, allocation-free
// after prepare().
//
// Four blocks — chorus, delay, reverb, stereo width — applied in a user-defined
// order. A disabled block is skipped entirely (no CPU, like an oscillator kill
// switch), so the cost scales with what's actually on.
//
// REORDERING / TOGGLING is click-free via a ~30 ms equal-power crossfade between
// two internal chain copies: on any configuration change the standby copy inherits
// the active copy's state, adopts the new order/enables, and the two are blended
// over the window before the standby becomes active. The doubled cost only lasts
// for the fade, which happens on a user gesture — never in steady state.
//
// Effect indices (order[] is a permutation of these):
//   0 = chorus, 1 = delay, 2 = reverb, 3 = width
// ============================================================================

struct FXParams
{
    // Continuous params (smoothed inside each effect — no crossfade needed).
    float chorusRate = 0.8f, chorusDepth = 0.5f, chorusMix = 0.5f;
    float delayTimeMs = 300.0f, delayFeedback = 0.35f, delayMix = 0.35f, delaySpread = 1.0f;
    float reverbSize = 0.5f, reverbDamp = 0.5f, reverbWidth = 1.0f, reverbMix = 0.3f;
    float width = 1.4f;

    // Structural config (a change here triggers the crossfade).
    bool enabled[4] { false, false, false, false };
    int  order[4]   { 0, 1, 2, 3 };
};

class FXChain
{
public:
    enum FX { Chorus_ = 0, Delay_ = 1, Reverb_ = 2, Width_ = 3, kNumFX = 4 };

    void prepare (double newSampleRate, int maxBlock)
    {
        a.prepare (newSampleRate, maxBlock);
        b.prepare (newSampleRate, maxBlock);
        scratchL.assign ((size_t) maxBlock, 0.0f);
        scratchR.assign ((size_t) maxBlock, 0.0f);
        xfadeLen = std::max (1, (int) (newSampleRate * 0.030));   // 30 ms
        active = &a; standby = &b;
        reset();
    }

    void reset()
    {
        a.reset(); b.reset();
        xfadePos = 0;
        crossfading = false;
    }

    // Push the full parameter set. Continuous params go to both copies every call
    // (they smooth internally). A change to order/enables is NOT applied to the
    // active chain here — process() detects it and crossfades it in, so the active
    // config only ever changes at a fade boundary. Applying it eagerly would
    // hard-switch the routing and click.
    void setParams (const FXParams& p)
    {
        desired = p;
        a.setContinuous (p);
        b.setContinuous (p);
    }

    void process (float* left, float* right, int numSamples)
    {
        if (! crossfading && configDiffers (active->order, active->enabled, desired.order, desired.enabled))
            startCrossfade();

        if (! crossfading)
        {
            active->process (left, right, numSamples);
            return;
        }

        // Crossfade: A in place, B on a copy of the same input, then blend.
        std::copy (left,  left  + numSamples, scratchL.begin());
        std::copy (right, right + numSamples, scratchR.begin());
        active->process (left, right, numSamples);
        standby->process (scratchL.data(), scratchR.data(), numSamples);

        for (int i = 0; i < numSamples; ++i)
        {
            const float t  = std::min (1.0f, (float) (xfadePos + i) / (float) xfadeLen);
            const float gA = std::cos (t * kHalfPi);      // equal-power
            const float gB = std::sin (t * kHalfPi);
            left[i]  = left[i]  * gA + scratchL[(size_t) i] * gB;
            right[i] = right[i] * gA + scratchR[(size_t) i] * gB;
        }

        xfadePos += numSamples;
        if (xfadePos >= xfadeLen)
        {
            std::swap (active, standby);   // B is now the live chain
            crossfading = false;
        }
    }

    bool isCrossfading() const { return crossfading; }

private:
    // One full copy of the four effects plus its own order/enable config.
    struct Chain
    {
        Chorus chorus; StereoDelay delay; Reverb reverb; StereoWidth width;
        int  order[4]   { 0, 1, 2, 3 };
        bool enabled[4] { false, false, false, false };

        void prepare (double sr, int maxBlock)
        {
            chorus.prepare (sr, maxBlock);
            delay.prepare (sr, maxBlock);
            reverb.prepare (sr);
            width.prepare (sr);
        }
        void reset() { chorus.reset(); delay.reset(); reverb.reset(); width.reset(); }

        void setContinuous (const FXParams& p)
        {
            chorus.setParams (p.chorusRate, p.chorusDepth, p.chorusMix);
            delay.setParams  (p.delayTimeMs, p.delayFeedback, p.delayMix, p.delaySpread);
            reverb.setParams (p.reverbSize, p.reverbDamp, p.reverbWidth, p.reverbMix);
            width.setWidth   (p.width);
        }
        void setConfig (const int ord[4], const bool en[4])
        {
            for (int i = 0; i < 4; ++i) { order[i] = ord[i]; enabled[i] = en[i]; }
        }
        void copyStateFrom (const Chain& o)
        {
            chorus.copyStateFrom (o.chorus);
            delay.copyStateFrom  (o.delay);
            reverb.copyStateFrom (o.reverb);
            width.copyStateFrom  (o.width);
        }

        void process (float* L, float* R, int n)
        {
            for (int slot = 0; slot < 4; ++slot)
            {
                const int fx = order[slot];
                if (! enabled[fx]) continue;               // skipped -> free
                switch (fx)
                {
                    case Chorus_: chorus.process (L, R, n); break;
                    case Delay_:  delay.process  (L, R, n); break;
                    case Reverb_: reverb.process (L, R, n); break;
                    case Width_:  width.process  (L, R, n); break;
                    default: break;
                }
            }
        }
    };

    void startCrossfade()
    {
        standby->copyStateFrom (*active);
        standby->setContinuous (desired);
        standby->setConfig (desired.order, desired.enabled);
        xfadePos = 0;
        crossfading = true;
    }

    static bool configDiffers (const int oA[4], const bool eA[4],
                               const int oB[4], const bool eB[4])
    {
        for (int i = 0; i < 4; ++i)
            if (oA[i] != oB[i] || eA[i] != eB[i]) return true;
        return false;
    }

    static constexpr float kHalfPi = 1.5707963267948966f;

    Chain a, b;
    Chain* active  = &a;
    Chain* standby = &b;
    std::vector<float> scratchL, scratchR;
    int  xfadeLen = 1440;
    int  xfadePos = 0;
    bool crossfading = false;
    FXParams desired;
};
