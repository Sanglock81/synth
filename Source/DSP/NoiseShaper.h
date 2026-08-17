// Synth — Copyright © 2026 John L Farmer. Licensed under AGPLv3; see LICENSE.
#pragma once
#include <cmath>
#include <algorithm>
#include <cstdint>

// ============================================================================
// NOISE XY — one continuous shaping field for the 4th source (white noise).
// JUCE-free, RT-safe (no alloc, no lock, fixed state), one instance per voice.
//
//   y = 0        TILT       x sweeps spectral tilt:  brown <- pink <- white -> bright
//   y > 0        FOCUS      x sweeps a bandpass centre (40 Hz .. 12 kHz),
//                           y sweeps its Q (very wide -> near-ring whistle)
//   y in [0,.15] the two regions crossfade, so the surface is ONE field with no seam.
//
// CHARACTER OVER ACCURACY. The tilt network is a 3-stage cascade, so its slope is a
// staircase approximation with a few dB of ripple between the anchors — it sounds like
// the colour it is named after; it is NOT a calibrated pink/brown reference generator.
// User-facing docs must not claim exact dB/oct slopes.
//
// BYPASS: at the exact defaults (x = 0.5, y = 0) with no modulation the CALLER skips
// this class entirely (SynthVoice takes a true code-path bypass), so a patch that never
// touches the field renders bit-identically to before the feature existed.
//
// ---- TILT (region 1) --------------------------------------------------------
// Three cascaded first-order shelving sections, anchored a decade apart (40 / 400 /
// 4000 Hz) so together they span the audible band. Each section carries one third of
// the requested tilt: its pole and zero sit symmetrically around its anchor, spread by
// exactly the gain it must deliver. That construction has two properties we rely on:
//   * at zero tilt pole == zero, so every section is EXACTLY unity (no residue), and
//   * |a1| < 1 for any positive cutoff, so it is unconditionally stable at any rate.
// (Paul Kellet's fixed-coefficient pinking filter is the ancestor of this idea; the
// coefficients here are computed from the sample rate instead, so the colour does not
// slide up the spectrum at 96/192 kHz the way his 44.1 kHz constants do.)
//
// ---- FOCUS (region 2) -------------------------------------------------------
// A dedicated TPT state-variable bandpass (Cytomic form) — deliberately NOT the shared
// SVFilter, which carries in-loop drive, self-oscillation seeding and oversampling that
// this path neither needs nor should pay for, and whose behaviour must not change for
// its existing users. Zero-delay feedback keeps it stable at any Q and any rate.
// ============================================================================

class NoiseShaper
{
public:
    // Exact bypass coordinates. A field parked here (with no matrix route) is skipped
    // wholesale by the caller — the guarantee behind the golden-render proof.
    static constexpr float kDefaultX = 0.5f;
    static constexpr float kDefaultY = 0.0f;
    static bool isBypass (float x, float y) { return x == kDefaultX && y == kDefaultY; }

    // ---- field geometry (also the single source of truth for the UI readout) ----
    static constexpr float kFocusMinHz  = 40.0f;      // focus centre at x = 0
    static constexpr float kFocusMaxHz  = 12000.0f;   // focus centre at x = 1
    static constexpr float kFocusMinQ   = 0.5f;       // y -> 0+ : very wide
    static constexpr float kFocusMaxQ   = 60.0f;      // y  = 1  : near-ring (pitched-noise whistle)
    static constexpr float kBlendTopY   = 0.15f;      // tilt -> focus crossfade ends here

    // Spectral tilt in dB/oct for a field x (region 1). Anchors are exact:
    // 0.0 -> -6 (brown), 0.25 -> -3 (pink), 0.5 -> 0 (white), 1.0 -> +4.5 (bright).
    static float tiltDbPerOct (float x)
    {
        const float d = std::clamp (x, 0.0f, 1.0f) - 0.5f;
        return d * (d < 0.0f ? 12.0f : 9.0f);
    }
    static float focusHz (float x)
    { return kFocusMinHz * std::pow (kFocusMaxHz / kFocusMinHz, std::clamp (x, 0.0f, 1.0f)); }
    static float focusQ (float y)
    { return kFocusMinQ * std::pow (kFocusMaxQ / kFocusMinQ, std::clamp (y, 0.0f, 1.0f)); }
    // Crossfade weight: 0 = pure tilt, 1 = pure focus.
    static float focusBlend (float y)
    { return std::clamp (y / kBlendTopY, 0.0f, 1.0f); }

    // ---- level trims -----------------------------------------------------------
    // TUNING CONSTANTS. These set how LOUD each part of the field is against the plain
    // white noise it replaces — not what it sounds like. The goal they serve: dragging
    // around the surface must change COLOUR, not volume, so the noise level knob keeps
    // meaning what it says wherever the field is parked. John tunes these by ear after
    // rc1; the "output level stays in one band" test is the guard rail that keeps a
    // future tuning pass from reintroducing a lurch.
    //
    // TILT. The shelf cascade is normalised to unity at the middle anchor, which lines up
    // the audible BODY of every colour — but not their loudness: noise power piles up
    // where the bandwidth is, so a bright tilt would still arrive ~21 dB hotter than white
    // and pink ~8 dB colder. kTiltRmsBright / kTiltRmsDark cancel that. They are EMPIRICAL
    // (fitted to the measured RMS of the real filter across the axis, hence a linear law on
    // the bright side and a bowl on the dark side, where brown's subsonic energy climbs
    // back toward white's). Retune by measuring, not by reasoning about the shape.
    static constexpr float kTiltMakeup    = 1.0f;    // overall tilt-region trim
    static constexpr float kTiltRmsBright = 1.43f;   // dB-per-unit-exponent, rising side
    static constexpr float kTiltRmsDark   = 1.66f;   // bowl coefficient, falling side
    // FOCUS. A bandpass passes only its bandwidth, so its RMS runs as sqrt(centre / Q) —
    // and, for a fixed-Hz band fed by per-sample white noise, as sqrt(sampleRate). The
    // sqrt-law trim cancels all three; kFocusMakeup then lifts the whole region onto the
    // same loudness as the tilt row so crossing y = 0.15 is not a step. The resonant PEAK
    // is deliberately left intact — that peak is the near-ring whistle.
    static constexpr float kFocusMakeup = 3.85f;
    static constexpr float kFocusRefHz  = 1000.0f;    // reference centre for the sqrt-law trim
    static constexpr float kFocusRefSR  = 48000.0f;   // reference rate for the sqrt-law trim

    void prepare (double sr)
    {
        sampleRate = sr > 0.0 ? sr : 48000.0;
        // ~10 ms one-pole on the field coordinates. This is what makes a MODULATED field
        // safe: an LFO stepping Q on a ringing bandpass once per block clicks; gliding the
        // coordinate between coefficient updates does not.
        smoothCoef = 1.0f - std::exp (-(float) kUpdateInterval / (0.010f * (float) sampleRate));
        reset();
        snapNext = true;
    }

    // Drop all filter state (call when the shaper re-enters the signal path, where its
    // contribution is weighted ~0 and a reset is inaudible).
    void reset()
    {
        for (auto& s : tilt) { s.x1 = 0.0f; s.y1 = 0.0f; }
        ic1 = 0.0f; ic2 = 0.0f;
        counter = 0;
        tiltCoefX = focusCoefX = focusCoefY = -1.0f;
    }

    // Control-rate: the field coordinate this chunk is heading for. Cheap — the actual
    // coefficient work happens inside process() at the update interval.
    void setTarget (float x, float y)
    {
        targetX = std::clamp (x, 0.0f, 1.0f);
        targetY = std::clamp (y, 0.0f, 1.0f);
        if (snapNext) { curX = targetX; curY = targetY; snapNext = false; }
    }

    // Jump straight to the target (a fresh note: no glide from the previous note's field).
    void snapToTarget() { snapNext = true; }

    float process (float white)
    {
        if (counter == 0)
        {
            curX += smoothCoef * (targetX - curX);
            curY += smoothCoef * (targetY - curY);
            updateCoefficients();
        }
        counter = (counter + 1) & (kUpdateInterval - 1);

        float out = 0.0f;
        if (blend < 1.0f) out += (1.0f - blend) * processTilt (white);
        if (blend > 0.0f) out += blend * processFocus (white);
        return out;
    }

private:
    static constexpr int   kUpdateInterval = 16;    // samples between coefficient recomputes (power of 2)
    static constexpr int   kTiltStages     = 3;
    static constexpr float kTiltAnchorHz   = 40.0f; // lowest shelf anchor; the others are a decade up each
    static constexpr float kTiltSpacing    = 10.0f; // anchor ratio (3.32 octaves) — 3 stages span ~10 octaves
    static constexpr float kDenormal       = 1.0e-20f;

    struct Shelf { float b0 = 1.0f, b1 = 0.0f, a1 = 0.0f, x1 = 0.0f, y1 = 0.0f; };

    void updateCoefficients()
    {
        blend = focusBlend (curY);

        // Only rebuild what actually moved. A field parked anywhere (the overwhelmingly
        // common case, since only a modulated field changes) pays for its coefficients once
        // and then costs nothing but the filter arithmetic — which matters because these
        // branches carry several transcendentals and run per voice.
        if (blend < 1.0f && curX != tiltCoefX)
        {
            tiltCoefX = curX;
            // Exponent of the target power law: |H(f)| proportional to f^expo.
            const float expo = tiltDbPerOct (curX) / 6.0205999f;
            // Each stage delivers one third of the tilt, its pole/zero split symmetrically
            // around the anchor so the transition is centred there (minimises the staircase
            // ripple between anchors). Pole above zero = rising = bright; below = dark.
            const float spread = std::pow (kTiltSpacing, expo * 0.5f);
            float anchor = kTiltAnchorHz;
            for (int i = 0; i < kTiltStages; ++i)
            {
                setShelf (tilt[(std::size_t) i], anchor * spread, anchor / spread);
                anchor *= kTiltSpacing;
            }
            // -1.5 * expo pins unity at the middle anchor (the cascade has passed 1.5 stages
            // of tilt there); the second term is the measured RMS correction on top of it.
            const float rms = expo >= 0.0f ? kTiltRmsBright * expo
                                           : kTiltRmsDark * expo * (1.0f + expo);
            tiltGain = kTiltMakeup * std::pow (kTiltSpacing, -1.5f * expo - rms);
        }

        if (blend > 0.0f && (curX != focusCoefX || curY != focusCoefY))
        {
            focusCoefX = curX; focusCoefY = curY;
            const float fc = std::min (focusHz (curX), (float) (0.45 * sampleRate));
            const float q  = focusQ (curY);
            svfG = (float) std::tan (3.14159265358979324 * (double) fc / sampleRate);
            svfK = 1.0f / q;
            const float d = 1.0f / (1.0f + svfG * (svfG + svfK));
            svfA1 = d; svfA2 = svfG * d; svfA3 = svfG * svfA2;
            focusGain = kFocusMakeup * std::sqrt ((kFocusRefHz / (q * fc))
                                                  * ((float) sampleRate / kFocusRefSR));
        }
    }

    // First-order shelf, bilinear-transformed with prewarping. fp == fz collapses to an
    // exact identity (b0 = 1, b1 = a1), which is why zero tilt costs nothing in accuracy.
    void setShelf (Shelf& s, float fp, float fz)
    {
        const double nyq = 0.49 * sampleRate;
        const double kp  = std::tan (3.14159265358979324 * std::min ((double) fp, nyq) / sampleRate);
        const double kz  = std::tan (3.14159265358979324 * std::min ((double) fz, nyq) / sampleRate);
        const double g   = kp / kz;
        s.b0 = (float) (g * (kz + 1.0) / (kp + 1.0));
        s.b1 = (float) (g * (kz - 1.0) / (kp + 1.0));
        s.a1 = (float) ((kp - 1.0) / (kp + 1.0));
    }

    float processTilt (float white)
    {
        float v = white;
        for (auto& s : tilt)
        {
            const float y = s.b0 * v + s.b1 * s.x1 - s.a1 * s.y1 + kDenormal;
            s.x1 = v; s.y1 = y;
            v = y;
        }
        return v * tiltGain;
    }

    float processFocus (float white)
    {
        // TPT SVF, zero-delay feedback (Cytomic). v1 is the band output; its peak gain is Q,
        // which the sqrt-law trim scales but does not flatten — the ring is the point.
        const float v3 = white - ic2;
        const float v1 = svfA1 * ic1 + svfA2 * v3;
        const float v2 = ic2 + svfA2 * ic1 + svfA3 * v3;
        ic1 = 2.0f * v1 - ic1 + kDenormal;
        ic2 = 2.0f * v2 - ic2 + kDenormal;
        // At the very top of the Q range the loop rings hard; a non-finite state would
        // otherwise persist for the life of the note. Cheap, and never taken in practice.
        if (! std::isfinite (ic1) || ! std::isfinite (ic2)) { ic1 = 0.0f; ic2 = 0.0f; return 0.0f; }
        return v1 * focusGain;
    }

    Shelf tilt[kTiltStages];
    float tiltGain  = 1.0f;
    // Coordinates the live coefficients were built for; a sentinel outside 0..1 forces the
    // first update (and reset() re-arms it, so a re-entering voice never reuses stale ones).
    float tiltCoefX = -1.0f, focusCoefX = -1.0f, focusCoefY = -1.0f;

    float ic1 = 0.0f, ic2 = 0.0f;                       // SVF integrator state
    float svfG = 0.0f, svfK = 1.0f;
    float svfA1 = 1.0f, svfA2 = 0.0f, svfA3 = 0.0f;
    float focusGain = 1.0f;

    double sampleRate = 48000.0;
    float  targetX = kDefaultX, targetY = kDefaultY;
    float  curX    = kDefaultX, curY    = kDefaultY;
    float  smoothCoef = 1.0f;
    float  blend   = 0.0f;
    int    counter = 0;
    bool   snapNext = true;
};
