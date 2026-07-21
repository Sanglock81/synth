#pragma once
#include <cmath>
#include <algorithm>
#include <cstdint>

// ============================================================================
// State-Variable Filter, TPT ("topology-preserving transform") form,
// after Andrew Simper (Cytomic). Hand-rolled.
//
// WHY THIS TOPOLOGY:
// The naive digital SVF blows up at high cutoff frequencies and behaves badly
// under fast modulation. The TPT/zero-delay-feedback formulation solves the
// implicit feedback equation analytically, so it:
//   * stays stable across the whole audible range,
//   * responds smoothly to fast cutoff sweeps (filter envelopes, LFOs) —
//     exactly the moves that define an analog synth,
//   * gives LP / HP / BP / Notch simultaneously from the same state,
//     which is how we get multimode for free.
//
// Resonance here maps 0..1 -> k = 2..0 (k is damping). At res = 1 the filter
// sits on the edge of self-oscillation; the LINEAR path clamps slightly below
// to keep it well-behaved.
//
// DRIVE + SELF-OSC (Musicality Pass, Tier 2). The celebrated analog filters get
// their sound from saturation INSIDE the loop, not a waveshaper in front
// (Huovilainen; the Korg 35's diode clipper bounds the feedback). So `drive`
// (0..1) adds an IN-LOOP nonlinearity: the driven input is soft-clipped (tanh)
// and the bandpass integrator state — the signal that feeds resonance back — is
// bounded by tanh too. That colours the passband and tames screaming resonance.
// The same bounded loop lets resonance past unity SELF-OSCILLATE gracefully into
// a keytracked sine at cutoff (a playable instrument), seeded by an inaudible
// noise floor so it blooms even from silence — see setCutoff.
//
// drive == 0 takes a fast path that is LITERALLY the old linear code — bit-exact,
// so goldens and the ThinkPad budget are unchanged; you pay for the tanh evals
// only when the filter is actually driven. tanh cost dominates the nonlinear
// path, so it uses a fast rational approximation (tanhFast, tested to a tight
// tolerance against std::tanh).
// ============================================================================

class SVFilter
{
public:
    enum class Type { LowPass, HighPass, BandPass, Notch };

    // Fast rational tanh: Pade [3/2] inside |x|<=3, hard-limited to +/-1 beyond
    // (tanh(3)=0.995). Monotonic, odd, and bounded to +/-1 — the three properties
    // the in-loop saturator needs for stability. It is within ~0.024 of std::tanh
    // (a slight mid-range overshoot near |x|~2); that 2% deviation in the knee is
    // sonically negligible for a soft saturation, and the DSP suite pins the bound.
    static inline double tanhFast (double x) noexcept
    {
        if (x < -3.0) return -1.0;
        if (x >  3.0) return  1.0;
        const double x2 = x * x;
        return x * (27.0 + x2) / (27.0 + 9.0 * x2);
    }

    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate;
        reset();
    }

    void reset() { ic1eq = ic2eq = 0.0; }

    void setType (Type t) { type = t; }

    // Drive amount 0..1. 0 -> the bit-exact linear fast path; >0 engages the
    // in-loop tanh nonlinearity with up to kMaxDriveGain of input gain.
    void setDrive (float drive01)
    {
        const float d = std::clamp (drive01, 0.0f, 1.0f);
        nonlinear  = d > 0.0f;
        driveGain  = 1.0 + (double) d * (kMaxDriveGain - 1.0);
        driveComp  = 1.0 / driveGain;      // small-signal makeup: driving doesn't just raise level
    }

    // cutoff in Hz, resonance 0..1
    void setCutoff (double cutoffHz, double resonance)
    {
        cutoffHz  = std::clamp (cutoffHz, 20.0, sampleRate * 0.49);
        resonance = std::clamp (resonance, 0.0, 1.0);

        // Resonance -> damping k. The historical range [0, kResLinearMax] is UNCHANGED
        // (k = 2 - 2*res), so old presets are bit-exact — filterReso's 0..1 range is
        // frozen and only the top sliver changes meaning. That sliver (kResLinearMax, 1]
        // opens SELF-OSCILLATION: k continues below 0.04 to a slightly negative floor so
        // the saturator-bounded loop blooms into a sine at cutoff. A ramped makeup lifts
        // the (otherwise quiet) self-osc output to a usable level — unity at the boundary,
        // so a resonance sweep crosses into self-osc with no level jump.
        if (resonance <= kResLinearMax)
        {
            k = 2.0 - 2.0 * resonance;                             // 2 .. 0.04 (unchanged)
            selfOsc = false;
            selfOscComp = 1.0;
        }
        else
        {
            const double t = (resonance - kResLinearMax) / (1.0 - kResLinearMax);   // 0..1 across sliver
            k = 0.04 + t * (kSelfOscKmin - 0.04);                 // 0.04 .. kSelfOscKmin (negative)
            selfOsc = true;
            selfOscComp = 1.0 + t * (kSelfOscMakeup - 1.0);
        }

        g  = std::tan (pi * cutoffHz / sampleRate);
        a1 = 1.0 / (1.0 + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    float process (float input)
    {
        if (! nonlinear && ! selfOsc)
            return processLinear (input);      // bit-exact legacy path (drive == 0, res <= sliver)

        // --- driven and/or self-oscillating: in-loop nonlinearity ---------
        // Soft-clip the driven input, run the TPT solve, then bound the bandpass
        // integrator state (the resonance-feedback signal) through tanh. When self-
        // oscillating, seed the loop with an inaudible (~-120 dB) noise floor so a
        // silent filter still blooms — the digital analogue of analog thermal noise.
        // Integrator states are flushed to kill denormals on decaying tails.
        double x = static_cast<double> (input);
        if (selfOsc) x += kSelfOscSeed * seedNoise();
        const double v0 = tanhFast (x * driveGain);
        const double v3 = v0 - ic2eq;
        const double v1 = a1 * ic1eq + a2 * v3;
        const double v2 = ic2eq + a2 * ic1eq + a3 * v3;

        // ic1 (the bandpass / resonance state) is bounded by tanh. ic2 (the lowpass
        // integrator) has NO damping when k < 0 (self-osc), so a pathological input can
        // wind it up — clamp it well above any musical level (self-osc sits at ~1) so the
        // whole bounded path is provably finite and can never feed inf/NaN downstream.
        ic1eq = flushDenorm (tanhFast (2.0 * v1 - ic1eq));   // saturate the resonance path
        ic2eq = flushDenorm (std::clamp (2.0 * v2 - ic2eq, -kStateMax, kStateMax));

        const double low   = v2;
        const double band  = v1;
        const double high  = v0 - k * v1 - v2;

        double out = low;
        switch (type)
        {
            case Type::LowPass:  out = low;            break;
            case Type::HighPass: out = high;           break;
            case Type::BandPass: out = band;           break;
            case Type::Notch:    out = low + high;     break;
        }
        // Final safety bound on the driven/self-osc path: musical output (self-osc ~0.5,
        // drive < 2) is far below kOutMax, so this only caps pathological fuzz states and
        // guarantees a sane, finite value reaches the rest of the chain.
        return static_cast<float> (std::clamp (out * driveComp * selfOscComp, -kOutMax, kOutMax));
    }

private:
    // Kill subnormals (denormal floats are ~100x slower to process). Only touches values
    // below -300 dB, so it is inaudible; used only on the nonlinear/self-osc path so the
    // linear fast path stays bit-exact.
    static inline double flushDenorm (double x) noexcept
    {
        return (std::abs (x) < 1.0e-15) ? 0.0 : x;
    }

    // Deterministic tiny white-noise source for the self-osc seed.
    double seedNoise() noexcept
    {
        seedRng ^= seedRng << 13; seedRng ^= seedRng >> 17; seedRng ^= seedRng << 5;
        return static_cast<double> (static_cast<std::int32_t> (seedRng)) / 2147483648.0;
    }

    float processLinear (float input)
    {
        const double v0 = static_cast<double> (input);
        const double v3 = v0 - ic2eq;
        const double v1 = a1 * ic1eq + a2 * v3;
        const double v2 = ic2eq + a2 * ic1eq + a3 * v3;

        // flushDenorm returns its argument UNCHANGED above 1e-15, so this stays bit-exact for
        // any real signal (the filter always sees a full-level oscillator); it only bites on a
        // tail decaying through silence — e.g. a self-osc note whose resonance was pulled back.
        ic1eq = flushDenorm (2.0 * v1 - ic1eq);
        ic2eq = flushDenorm (2.0 * v2 - ic2eq);

        const double low   = v2;
        const double band  = v1;
        const double high  = v0 - k * v1 - v2;

        double out = low;
        switch (type)
        {
            case Type::LowPass:  out = low;            break;
            case Type::HighPass: out = high;           break;
            case Type::BandPass: out = band;           break;
            case Type::Notch:    out = low + high;     break;
        }
        return static_cast<float> (out);
    }

    static constexpr double pi = 3.141592653589793;
    static constexpr double kMaxDriveGain = 4.0;   // input gain at drive = 1 (~+12 dB into the saturator);
                                                   // chosen with the 1/driveGain makeup to keep a single-
                                                   // voice-level signal within ~2 dB across the drive range
    // Self-oscillation (2B). The historical resonance range ends at kResLinearMax; above it,
    // k ramps to kSelfOscKmin (negative -> the bounded loop blooms). Bloom reaches audible in
    // ~0.19 s at the floor; the seed is ~-120 dB; the makeup lifts the self-osc sine to a usable
    // level (steady self-osc ~0.12 * makeup). Pitch tracks cutoff to within ~0.5 cents (TPT prewarp).
    static constexpr double kResLinearMax = 0.98;  // top of the bit-exact linear resonance range
    static constexpr double kSelfOscKmin  = -0.12; // damping floor at resonance = 1.0 (reliable bloom)
    static constexpr double kSelfOscSeed  = 1.0e-6;// ~-120 dB thermal-noise seed (self-osc only)
    static constexpr double kSelfOscMakeup = 4.0;  // self-osc output makeup at resonance = 1.0
    static constexpr double kStateMax = 4.0;       // anti-windup clamp on the LP integrator (self-osc has no damping)
    static constexpr double kOutMax   = 8.0;       // final output safety bound on the driven/self-osc path

    Type   type = Type::LowPass;
    double sampleRate = 44100.0;
    double g = 0.0, k = 2.0, a1 = 0.0, a2 = 0.0, a3 = 0.0;
    double ic1eq = 0.0, ic2eq = 0.0;   // integrator states
    bool   nonlinear = false;          // drive > 0
    bool   selfOsc   = false;          // resonance in the self-oscillation sliver
    double driveGain = 1.0, driveComp = 1.0, selfOscComp = 1.0;
    std::uint32_t seedRng = 0x2545F491u;
};
