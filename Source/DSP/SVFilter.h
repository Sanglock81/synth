#pragma once
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <array>

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
        designHalfband();
        reset();
    }

    void reset()
    {
        ic1eq = ic2eq = 0.0;
        upPrev = 0.0;
        downZ.fill (0.0);
        downPos = 0;
    }

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

        // Coefficients are computed at the ACTUAL processing rate: 2x when the voice has
        // latched the oversampled path (see setOversample), else the base rate. The prewarp
        // keeps the resonant peak / self-osc frequency at `cutoffHz` in both domains.
        const double procSr = oversampled ? sampleRate * 2.0 : sampleRate;
        g  = std::tan (pi * cutoffHz / procSr);
        a1 = 1.0 / (1.0 + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    // Latch the oversampled path for this voice's note (see the header note on hysteresis).
    // Toggling resets the resampler state + integrators — done only at note boundaries by the
    // voice, so a rate-domain switch never happens mid-note. Coefficients pick up the new rate
    // on the next setCutoff (the voice always sets it before rendering a chunk).
    void setOversample (bool on)
    {
        if (on == oversampled) return;
        oversampled = on;
        reset();
    }

    float process (float input)
    {
        // A voice that latched oversampling runs the bounded core at 2x for the whole note
        // (coeffs are at 2x). Its drive can dip to 0 mid-note — driveGain->1 makes the core
        // nearly transparent — but the RATE never switches, so there is no discontinuity.
        if (oversampled)
        {
            // 2x: linear-interp upsample (input is already band-limited), two nonlinear-core
            // iterations, FIR half-band decimate. The makeup (driveComp * selfOscComp) is a
            // linear scale, so applying it once after decimation is equivalent to per-sample.
            const double u1 = static_cast<double> (input);
            const double u0 = 0.5 * (upPrev + u1);
            upPrev = u1;
            const double y0 = coreBounded (u0);
            const double y1 = coreBounded (u1);
            downPush (y0);
            const double out = downPush (y1);
            return static_cast<float> (std::clamp (out * driveComp * selfOscComp, -kOutMax, kOutMax));
        }

        if (! nonlinear && ! selfOsc)
            return processLinear (input);      // bit-exact legacy path (drive == 0, res <= sliver)

        // Bounded but not latched to oversample (e.g. drive automated up mid-note on a voice
        // that started clean): run the core once at base rate. Accepts a little aliasing for
        // that one note rather than switching rate domains mid-note.
        const double out = coreBounded (static_cast<double> (input));
        return static_cast<float> (std::clamp (out * driveComp * selfOscComp, -kOutMax, kOutMax));
    }

private:
    // One nonlinear TPT-SVF sample at the current coefficient rate, returning the type-selected
    // output BEFORE makeup/clamp (so the oversampler can apply those once after decimation).
    double coreBounded (double in)
    {
        // When self-oscillating, seed the loop with an inaudible (~-120 dB) noise floor so a
        // silent filter still blooms — the digital analogue of analog thermal noise.
        double x = in;
        if (selfOsc) x += kSelfOscSeed * seedNoise();
        const double v0 = tanhFast (x * driveGain);
        const double v3 = v0 - ic2eq;
        const double v1 = a1 * ic1eq + a2 * v3;
        const double v2 = ic2eq + a2 * ic1eq + a3 * v3;

        // ic1 (bandpass / resonance state) is bounded by tanh. ic2 (lowpass integrator) has NO
        // damping when k < 0 (self-osc), so clamp it well above any musical level -> the bounded
        // path is provably finite and can never feed inf/NaN downstream. Denormals flushed too.
        ic1eq = flushDenorm (tanhFast (2.0 * v1 - ic1eq));
        ic2eq = flushDenorm (std::clamp (2.0 * v2 - ic2eq, -kStateMax, kStateMax));

        const double low = v2, band = v1, high = v0 - k * v1 - v2;
        switch (type)
        {
            case Type::HighPass: return high;
            case Type::BandPass: return band;
            case Type::Notch:    return low + high;
            case Type::LowPass:
            default:             return low;
        }
    }

    // FIR half-band decimator: push one 2x-rate sample, return the filtered value. Half-band
    // taps at even offsets (except the center) are zero, so only the odd taps + center cost a
    // multiply. The voice keeps every SECOND return value (the decimated output).
    double downPush (double x)
    {
        downZ[(std::size_t) downPos] = x;
        double acc = hbCenter * downZ[(std::size_t) ((downPos - kHbCenterIdx + kHbLen) % kHbLen)];
        for (int j = 0; j < kHbNumOdd; ++j)
        {
            const int off = 2 * j + 1;                                  // 1, 3, 5, ...
            const int ia = (downPos - (kHbCenterIdx - off) + kHbLen) % kHbLen;
            const int ib = (downPos - (kHbCenterIdx + off) + kHbLen) % kHbLen;
            acc += hbOdd[(std::size_t) j] * (downZ[(std::size_t) ia] + downZ[(std::size_t) ib]);
        }
        downPos = (downPos + 1) % kHbLen;
        return acc;
    }
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

    // 2C: design the FIR half-band decimator taps (Hamming-windowed sinc, cutoff at a quarter
    // of the 2x rate = base Nyquist). Half-band => even-offset taps are zero, so only the
    // center + odd-offset taps are stored. Normalized to unity DC gain.
    void designHalfband()
    {
        double h[kHbLen]; double sum = 0.0;
        for (int n = 0; n < kHbLen; ++n)
        {
            const int m = n - kHbCenterIdx;
            const double s = (m == 0) ? 0.5 : std::sin (pi * 0.5 * m) / (pi * m);
            const double w = 0.54 - 0.46 * std::cos (2.0 * pi * n / (kHbLen - 1));
            h[n] = s * w; sum += h[n];
        }
        for (int n = 0; n < kHbLen; ++n) h[n] /= sum;
        hbCenter = h[kHbCenterIdx];
        for (int j = 0; j < kHbNumOdd; ++j) hbOdd[(std::size_t) j] = h[kHbCenterIdx + (2 * j + 1)];
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

    // 2C oversampling (latched per note by the voice). Contained 2x around JUST the filter:
    // linear-interp upsample + FIR half-band decimate. drive == 0 voices never engage it, so
    // the clean path stays bit-exact and pays nothing.
    static constexpr int kHbLen       = 11;   // FIR half-band length
    static constexpr int kHbCenterIdx = 5;    // (kHbLen-1)/2
    static constexpr int kHbNumOdd    = 3;    // non-zero odd-offset taps: offsets 1, 3, 5
    bool   oversampled = false;
    double upPrev = 0.0;                       // linear-interp upsampler state
    double hbCenter = 1.0, hbOdd[kHbNumOdd] = { 0.0, 0.0, 0.0 };
    std::array<double, kHbLen> downZ { };      // decimator delay line
    int    downPos = 0;
};
