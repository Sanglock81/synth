#pragma once
#include "Chorus.h"
#include "StereoDelay.h"
#include "Reverb.h"
#include "StereoWidth.h"
#include "PartEQ.h"
#include <vector>
#include <cmath>
#include <algorithm>

// ============================================================================
// Per-part reorderable stereo FX chain. Hand-rolled, JUCE-free, allocation-free
// after prepare().
//
// Four REORDERABLE blocks — chorus, delay, reverb, stereo width — applied in a
// user-defined order, plus the per-part EQ as a FIXED final stage. A disabled block
// is skipped entirely (no CPU, like an oscillator kill switch), so the cost scales
// with what's actually on.
//
// K1: the per-part EQ (index 4) is no longer reorderable — it always runs LAST,
// after every other block, regardless of where EQ_ sits in order[]. Its order[] slot
// is positionally inert (kept only so old presets deserialise unchanged and the
// enable still drives the crossfade). This is the whole "one EQ at the end of the
// part's chain" model; the UI drag panel exposes only the four reorderable FX.
//
// REORDERING / TOGGLING is click-free via a ~30 ms equal-power crossfade between
// two internal chain copies: on any configuration change the standby copy inherits
// the active copy's state, adopts the new order/enables, and the two are blended
// over the window before the standby becomes active. The doubled cost only lasts
// for the fade, which happens on a user gesture — never in steady state. EQ on/off is
// an enabled[] change, so it fades in/out click-free too.
//
// Effect indices (order[] is a permutation of these):
//   0 = chorus, 1 = delay, 2 = reverb, 3 = width, 4 = EQ (4-band per-part, fixed last)
// ============================================================================

struct FXParams
{
    // Continuous params (smoothed inside each effect — no crossfade needed).
    float chorusRate = 0.8f, chorusDepth = 0.5f, chorusMix = 0.5f;
    int   chorusVoices = 1;                          // Tier 4c: 1 or 2 taps
    float delayTimeMs = 300.0f, delayFeedback = 0.35f, delayMix = 0.35f, delaySpread = 1.0f;
    float reverbSize = 0.5f, reverbDamp = 0.5f, reverbWidth = 1.0f, reverbMix = 0.3f, reverbMotion = 0.0f;
    float width = 1.4f;
    float sat   = 0.0f;                               // tube saturation in the width block (0 = clean)
    PartEQ::Band eqBand1 { 180.0f,   0.0f, 0.9f };   // per-part EQ, 5 fully parametric bells (fixed last)
    PartEQ::Band eqBand2 { 1000.0f,  0.0f, 0.9f };
    PartEQ::Band eqBand3 { 5000.0f,  0.0f, 0.9f };
    PartEQ::Band eqBand4 { 10000.0f, 0.0f, 0.9f };
    PartEQ::Band eqBand5 { 14000.0f, 0.0f, 0.9f };

    // Structural config (a change here triggers the crossfade).
    bool enabled[5] { false, false, false, false, false };
    int  order[5]   { 0, 1, 2, 3, 4 };
};

class FXChain
{
public:
    enum FX { Chorus_ = 0, Delay_ = 1, Reverb_ = 2, Width_ = 3, EQ_ = 4, kNumFX = 5 };

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
    // One full copy of the five effects plus its own order/enable config.
    struct Chain
    {
        Chorus chorus; StereoDelay delay; Reverb reverb; StereoWidth width; PartEQ eq;
        int  order[kNumFX]   { 0, 1, 2, 3, 4 };
        bool enabled[kNumFX] { false, false, false, false, false };

        void prepare (double sr, int maxBlock)
        {
            chorus.prepare (sr, maxBlock);
            delay.prepare (sr, maxBlock);
            reverb.prepare (sr);
            width.prepare (sr);
            eq.prepare (sr);
        }
        void reset() { chorus.reset(); delay.reset(); reverb.reset(); width.reset(); eq.reset(); }

        void setContinuous (const FXParams& p)
        {
            chorus.setParams (p.chorusRate, p.chorusDepth, p.chorusMix, p.chorusVoices);
            delay.setParams  (p.delayTimeMs, p.delayFeedback, p.delayMix, p.delaySpread);
            reverb.setParams (p.reverbSize, p.reverbDamp, p.reverbWidth, p.reverbMix, p.reverbMotion);
            width.setWidth   (p.width);
            width.setSat     (p.sat);
            eq.setBands      (p.eqBand1, p.eqBand2, p.eqBand3, p.eqBand4, p.eqBand5);
        }
        void setConfig (const int ord[kNumFX], const bool en[kNumFX])
        {
            for (int i = 0; i < kNumFX; ++i) { order[i] = ord[i]; enabled[i] = en[i]; }
        }
        void copyStateFrom (const Chain& o)
        {
            chorus.copyStateFrom (o.chorus);
            delay.copyStateFrom  (o.delay);
            reverb.copyStateFrom (o.reverb);
            width.copyStateFrom  (o.width);
            eq.copyStateFrom     (o.eq);
        }

        void process (float* L, float* R, int n)
        {
            for (int slot = 0; slot < kNumFX; ++slot)
            {
                const int fx = order[slot];
                if (fx < 0 || fx >= kNumFX || ! enabled[fx]) continue;   // skipped -> free
                switch (fx)
                {
                    case Chorus_: chorus.process (L, R, n); break;
                    case Delay_:  delay.process  (L, R, n); break;
                    case Reverb_: reverb.process (L, R, n); break;
                    case Width_:  width.process  (L, R, n); break;
                    case EQ_:     break;   // K1: EQ is not reorderable — applied last, below
                    default: break;
                }
            }
            // Fixed final stage: the per-part EQ always runs after everything else,
            // ignoring its position in order[] (its enable still gates it click-free).
            if (enabled[EQ_]) eq.process (L, R, n);
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

    static bool configDiffers (const int oA[kNumFX], const bool eA[kNumFX],
                               const int oB[kNumFX], const bool eB[kNumFX])
    {
        for (int i = 0; i < kNumFX; ++i)
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
