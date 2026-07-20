#pragma once
#include <cmath>
#include <array>

// ============================================================================
// PolyBLEP anti-aliased oscillator (hand-rolled), oversampled + decimated.
//
// WHY THIS EXISTS:
// A naive digital saw wave is a hard discontinuity: it jumps from +1 to -1
// instantly. That jump contains energy above Nyquist, which folds back down as
// inharmonic garbage ("aliasing") — the classic cheap-digital sound.
//
// PolyBLEP (Polynomial Band-Limited stEP) replaces the sample or two around
// each discontinuity with a polynomial correction. On its own (1x) it reaches
// ~-26 dB aliasing at 3 kHz. To make aliasing inaudible we run the band-limited
// core at N x the sample rate and decimate with a linear-phase FIR.
//
// QUALITY MODES (set before prepare(); see SynthEngine::setOscQuality):
//   * Efficient — 4x + 48-tap FIR. Audible band (<=18 kHz) below -60 dB for a
//     3 kHz saw. Cheap enough for a 2-core ThinkPad. DEFAULT.
//   * HQ        — 4x + long FIR. Full band (<=23 kHz) below -60 dB. ~5x the
//     decimation cost; for studio / Windows use.
// The residual top-octave aliasing in Efficient mode (~-35 dB above 18 kHz) is
// above typical hearing; HQ removes it at real CPU cost. (Higher oversampling
// does NOT help the top octave — it makes the fixed 24 kHz transition a
// narrower normalized fraction — so both modes use 4x and differ in FIR length.)
//
//  * Saw:      one discontinuity per cycle  -> one BLEP correction
//  * Square:   two discontinuities per cycle -> two BLEP corrections (PWM free)
//  * Triangle: leaky-integrated BLEP'd square, leak = a fixed ~5 Hz DC blocker
//              scaled to the sample rate, so amplitude no longer drifts w/ pitch.
//  * Sine:     no discontinuities -> generated directly at base rate.
//
// NOTE: all state (phase, integrator, decimation ring) is per-instance, so each
// voice's two oscillators filter independently — nothing is shared across voices.
// ============================================================================

class PolyBlepOscillator
{
public:
    enum class Wave { Saw, Square, Triangle, Sine };

    // None      — 1x, no decimation. Lowest CPU, audible aliasing (~-26 dB @ 3 kHz).
    //             The skeleton's original behaviour; a benchmark/extreme-CPU baseline.
    // Efficient — 4x + 48-tap FIR. Audible band (<=18 kHz) below -60 dB. DEFAULT.
    // HQ        — 4x + 320-tap FIR. Full band (<=23 kHz) below -60 dB.
    enum class Quality { None, Efficient, HQ };

    static constexpr int kMaxFirLen = 320;     // HQ; ring buffers sized to this

    void setQuality (Quality q) { quality = q; }   // call before prepare()

    void prepare (double newSampleRate)
    {
        baseRate = newSampleRate;

        // Per-mode: (oversample, firLen, cutoff as fraction of osRate, Kaiser beta)
        switch (quality)
        {
            case Quality::HQ:        oversample = 4; firLen = 320; break;
            case Quality::Efficient: oversample = 4; firLen = 48;  break;
            case Quality::None:      oversample = 1; firLen = 1;   break;
        }
        osRate = newSampleRate * oversample;

        if      (quality == Quality::HQ)        designDecimator (22500.0 / osRate, 9.0);
        else if (quality == Quality::Efficient) designDecimator (0.45 / oversample, 7.0);
        else                                    h[0] = 1.0f;       // 1-tap identity

        reset();
    }

    // startPhase: 0 = RESET (today), in (0,1) = RANDOM start, < 0 = FREE (keep the running phase).
    // The oversampling ring history is always cleared for a fresh voice (avoids stale FIR state).
    void reset (double startPhase = 0.0)
    {
        if (startPhase >= 0.0) phase = startPhase - std::floor (startPhase);
        ring.fill (0.0);
        ringPos = 0;
    }

    void setFrequency (double hz)          { phaseInc = hz / osRate; }   // per oversample step
    void setWave (Wave w)                  { wave = w; }
    void setPulseWidth (double pw)         { pulseWidth = pw; }          // 0.05 .. 0.95

    // Render one base-rate sample. Called per-sample from the voice.
    float nextSample()
    {
        if (wave == Wave::Sine)
        {
            // No aliasing to correct: run at base rate, skip oversampling.
            const double out = std::sin (phase * kTwoPi);
            phase += phaseInc * oversample;                   // = hz / baseRate
            if (phase >= 1.0) phase -= 1.0;
            return static_cast<float> (out);
        }

        if (oversample == 1)
            return static_cast<float> (coreSample());         // None: direct polyBLEP

        for (int k = 0; k < oversample; ++k)
            pushRing (coreSample());

        return static_cast<float> (decimate());
    }

private:
    static constexpr double kTwoPi = 6.283185307179586;
    static constexpr double kPi    = 3.141592653589793;

    static double wrap (double x) { return x >= 1.0 ? x - 1.0 : x; }

    // One band-limited sample at the oversampled rate; advances `phase`.
    double coreSample()
    {
        const double dt = phaseInc;
        double out = 0.0;

        switch (wave)
        {
            case Wave::Saw:
                out = 2.0 * phase - 1.0;
                out -= polyBlep (phase, dt);
                break;

            case Wave::Square:
                out = (phase < pulseWidth ? 1.0 : -1.0);
                out += polyBlep (phase, dt);                           // rising edge at 0
                out -= polyBlep (wrap (phase - pulseWidth + 1.0), dt); // falling edge at PW
                break;

            case Wave::Triangle:
            {
                // Direct piecewise-linear triangle, bounded [-1,1] by
                // construction (rises over [0,pw), falls over [pw,1) — pw gives
                // an asymmetric triangle). Its slope-corner aliasing is mild
                // (triangle harmonics fall as 1/k^2) and the 4x decimation cleans
                // it up. This avoids the leaky-integrator's droop/overshoot.
                // (A polyBLAMP-corrected triangle is a possible v2 refinement.)
                const double up  = phase / pulseWidth;
                const double dn  = (1.0 - phase) / (1.0 - pulseWidth);
                out = 2.0 * (phase < pulseWidth ? up : dn) - 1.0;
                break;
            }

            case Wave::Sine:   // handled in nextSample(); never reached here
                break;
        }

        phase += dt;
        if (phase >= 1.0) phase -= 1.0;
        return out;
    }

    // Two-sample polynomial BLEP residual around a discontinuity at phase==0.
    static double polyBlep (double t, double dt)
    {
        if (t < dt)               // just after the discontinuity
        {
            t /= dt;
            return t + t - t * t - 1.0;
        }
        if (t > 1.0 - dt)         // just before the discontinuity
        {
            t = (t - 1.0) / dt;
            return t * t + t + t + 1.0;
        }
        return 0.0;
    }

    // ---- decimation FIR ----------------------------------------------------
    void pushRing (double s)
    {
        const float f = (float) s;
        ring[(std::size_t) ringPos]              = f;
        ring[(std::size_t) (ringPos + firLen)]  = f;     // mirror -> contiguous window
        if (++ringPos == firLen) ringPos = 0;
    }

    // Float FIR: sample VALUES need only single precision (phase stays double);
    // the -76 dB aliasing floor is far above float epsilon, and the contiguous
    // float MAC vectorizes ~2x better than double.
    double decimate() const
    {
        // Last firLen samples sit contiguously at ring[ringPos .. +firLen-1],
        // oldest first. h is symmetric, so tap order is irrelevant.
        float acc = 0.0f;
        const float* w = &ring[(std::size_t) ringPos];
        for (int j = 0; j < firLen; ++j)
            acc += h[(std::size_t) j] * w[j];
        return acc;
    }

    // Windowed-sinc lowpass into h[0..firLen). fc normalized to osRate.
    void designDecimator (double fc, double beta)
    {
        const double M = (firLen - 1) / 2.0;
        double sum = 0.0;
        for (int n = 0; n < firLen; ++n)
        {
            const double m = n - M;
            const double sinc = (std::abs (m) < 1e-9) ? 2.0 * fc      // exact center tap
                                                      : std::sin (kTwoPi * fc * m) / (kPi * m);
            const double win  = i0 (beta * std::sqrt (std::max (0.0, 1.0 - (m / M) * (m / M)))) / i0 (beta);
            hd[(std::size_t) n] = sinc * win;
            sum += hd[(std::size_t) n];
        }
        for (int n = 0; n < firLen; ++n)
            h[(std::size_t) n] = (float) (hd[(std::size_t) n] / sum);   // unity DC gain, to float
    }

    // Modified Bessel function I0 via series (for the Kaiser window).
    static double i0 (double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 60; ++k)
        {
            const double t = (x / (2.0 * k));
            term *= t * t;
            sum += term;
            if (term < 1e-14 * sum) break;
        }
        return sum;
    }

    Wave    wave       = Wave::Saw;
    Quality quality    = Quality::Efficient;
    double  baseRate   = 44100.0;
    double  osRate     = 44100.0 * 4;
    double  phase      = 0.0;
    double  phaseInc   = 0.0;    // per oversample step
    double  pulseWidth = 0.5;
    int     oversample = 4;
    int     firLen     = 48;

    std::array<float,  kMaxFirLen>     h  {};            // decimation taps (symmetric, float MAC)
    std::array<double, kMaxFirLen>     hd {};            // design scratch (double, unused at RT)
    std::array<float,  2 * kMaxFirLen> ring {};          // mirrored delay line
    int                                ringPos = 0;
};
