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
// sits on the edge of self-oscillation; we clamp slightly below to keep v1
// well-behaved. Pushing into true self-oscillation is a v2 experiment.
// ============================================================================

class SVFilter
{
public:
    enum class Type { LowPass, HighPass, BandPass, Notch };

    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate;
        reset();
    }

    void reset() { ic1eq = ic2eq = 0.0; }

    void setType (Type t) { type = t; }

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

private:
    static constexpr double pi = 3.141592653589793;

    Type   type = Type::LowPass;
    double sampleRate = 44100.0;
    double g = 0.0, k = 2.0, a1 = 0.0, a2 = 0.0, a3 = 0.0;
    double ic1eq = 0.0, ic2eq = 0.0;   // integrator states
};
