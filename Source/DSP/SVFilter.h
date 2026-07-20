#pragma once
#include <cmath>
#include <algorithm>

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
// DRIVE (Musicality Pass, Tier 2). The celebrated analog filters get their
// sound from saturation INSIDE the loop, not a waveshaper in front (Huovilainen;
// the Korg 35's diode clipper bounds the feedback). So `drive` (0..1) adds an
// IN-LOOP nonlinearity: the driven input is soft-clipped (tanh) and the bandpass
// integrator state — the signal that feeds resonance back — is bounded by tanh
// too. That colours the passband, tames screaming resonance gracefully, and
// (once the resonance clamp is lifted, next increment) lets it self-oscillate.
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
        cutoffHz = std::clamp (cutoffHz, 20.0, sampleRate * 0.49);
        resonance = std::clamp (resonance, 0.0, 0.98);

        g  = std::tan (pi * cutoffHz / sampleRate);
        k  = 2.0 - 2.0 * resonance;
        a1 = 1.0 / (1.0 + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    float process (float input)
    {
        if (! nonlinear)
            return processLinear (input);      // bit-exact legacy path (drive == 0)

        // --- driven: in-loop nonlinearity ---------------------------------
        // Soft-clip the driven input, run the TPT solve, then bound the bandpass
        // integrator state (the resonance-feedback signal) through tanh. Output
        // is makeup-scaled so a quiet passband keeps its level.
        const double v0 = tanhFast (static_cast<double> (input) * driveGain);
        const double v3 = v0 - ic2eq;
        const double v1 = a1 * ic1eq + a2 * v3;
        const double v2 = ic2eq + a2 * ic1eq + a3 * v3;

        ic1eq = tanhFast (2.0 * v1 - ic1eq);   // saturate the resonance path
        ic2eq = 2.0 * v2 - ic2eq;

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
        return static_cast<float> (out * driveComp);
    }

private:
    float processLinear (float input)
    {
        const double v0 = static_cast<double> (input);
        const double v3 = v0 - ic2eq;
        const double v1 = a1 * ic1eq + a2 * v3;
        const double v2 = ic2eq + a2 * ic1eq + a3 * v3;

        ic1eq = 2.0 * v1 - ic1eq;
        ic2eq = 2.0 * v2 - ic2eq;

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

    Type   type = Type::LowPass;
    double sampleRate = 44100.0;
    double g = 0.0, k = 2.0, a1 = 0.0, a2 = 0.0, a3 = 0.0;
    double ic1eq = 0.0, ic2eq = 0.0;   // integrator states
    bool   nonlinear = false;          // drive > 0
    double driveGain = 1.0, driveComp = 1.0;
};
