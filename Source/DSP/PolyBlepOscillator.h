#pragma once
#include <cmath>

// ============================================================================
// PolyBLEP anti-aliased oscillator (hand-rolled).
//
// WHY THIS EXISTS:
// A naive digital saw wave is a hard discontinuity: it jumps from +1 to -1
// instantly. In the frequency domain, that jump contains energy above the
// Nyquist frequency, which folds back down as inharmonic garbage ("aliasing")
// — the classic cheap-digital sound.
//
// PolyBLEP (Polynomial Band-Limited stEP) fixes this by replacing the sample
// or two around each discontinuity with a small polynomial correction that
// approximates a band-limited step. It costs almost nothing per sample and
// gets us ~95% of the way to a "real" analog-sounding edge.
//
//  * Saw:      one discontinuity per cycle  -> one BLEP correction
//  * Square:   two discontinuities per cycle -> two BLEP corrections
//              (this also gives us PWM for free by moving the second edge)
//  * Triangle: derived by leaky-integrating the BLEP'd square. A triangle's
//              corners are derivative discontinuities (much gentler aliasing)
//              and integrating a band-limited square handles them elegantly.
//  * Sine:     no discontinuities, no correction needed.
// ============================================================================

class PolyBlepOscillator
{
public:
    enum class Wave { Saw, Square, Triangle, Sine };

    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate;
        reset();
    }

    void reset()
    {
        phase = 0.0;
        triState = 0.0;
    }

    void setFrequency (double hz)          { phaseInc = hz / sampleRate; }
    void setWave (Wave w)                  { wave = w; }
    void setPulseWidth (double pw)         { pulseWidth = pw; }   // 0.05 .. 0.95

    // Render one sample. Called per-sample from the voice.
    float nextSample()
    {
        double out = 0.0;

        switch (wave)
        {
            case Wave::Sine:
                out = std::sin (phase * juce_TwoPi);
                break;

            case Wave::Saw:
                // Naive saw in [-1, 1], then subtract the BLEP at the wrap.
                out = 2.0 * phase - 1.0;
                out -= polyBlep (phase);
                break;

            case Wave::Square:
                out = (phase < pulseWidth ? 1.0 : -1.0);
                out += polyBlep (phase);                              // rising edge at 0
                out -= polyBlep (wrap (phase - pulseWidth + 1.0));    // falling edge at PW
                break;

            case Wave::Triangle:
            {
                // BLEP'd square -> leaky integrator -> triangle.
                double sq = (phase < pulseWidth ? 1.0 : -1.0);
                sq += polyBlep (phase);
                sq -= polyBlep (wrap (phase - pulseWidth + 1.0));

                // The 4*phaseInc gain keeps amplitude roughly constant across
                // pitch; the 0.995 leak stops DC drift from accumulating.
                triState = 0.995 * triState + 4.0 * phaseInc * sq;
                out = triState;
                break;
            }
        }

        phase += phaseInc;
        if (phase >= 1.0)
            phase -= 1.0;

        return static_cast<float> (out);
    }

private:
    static constexpr double juce_TwoPi = 6.283185307179586;

    static double wrap (double x) { return x >= 1.0 ? x - 1.0 : x; }

    // Two-sample polynomial BLEP residual. `t` is phase in [0,1),
    // phaseInc is the normalized frequency. Returns the correction to add
    // around a discontinuity located at phase == 0.
    double polyBlep (double t) const
    {
        const double dt = phaseInc;

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

    Wave   wave       = Wave::Saw;
    double sampleRate = 44100.0;
    double phase      = 0.0;
    double phaseInc   = 0.0;
    double pulseWidth = 0.5;
    double triState   = 0.0;   // integrator state for triangle
};
