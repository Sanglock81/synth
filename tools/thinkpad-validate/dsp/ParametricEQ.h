#pragma once
#include <cmath>
#include <array>

// M_PI isn't standard (absent under MSVC without _USE_MATH_DEFINES); Source/DSP/ is
// dependency-free, so define our own.
namespace eqconst { inline constexpr double kPi = 3.14159265358979323846; }

// ============================================================================
// Master parametric EQ — JUCE-free (standard library only, like everything in
// Source/DSP/). Four RBJ biquad bands in series: low shelf, two sweepable bells,
// high shelf. Stereo (independent state per channel), transposed-direct-form-II so
// coefficient updates don't click. Coefficients recompute per block (cheap, off the
// per-sample path). With every gain at 0 dB the bands are unity, and the owner skips
// processing entirely when the EQ is off, so the signal is bit-identical until used.
// ============================================================================

class ParametricEQ
{
public:
    struct Band { float freq = 1000.0f, gainDb = 0.0f, q = 0.9f; };
    enum Type { LowShelf, Bell, HighShelf };

    void prepare (double sr)
    {
        sampleRate = sr > 0.0 ? sr : 48000.0;
        reset();
    }
    void reset() { for (auto& b : biquads) b.reset(); }

    // Set all four bands (low shelf, low-mid bell, high-mid bell, high shelf).
    void setBands (const Band& ls, const Band& lm, const Band& hm, const Band& hs)
    {
        biquads[0].set (LowShelf,  sampleRate, ls);
        biquads[1].set (Bell,      sampleRate, lm);
        biquads[2].set (Bell,      sampleRate, hm);
        biquads[3].set (HighShelf, sampleRate, hs);
    }

    void process (float* L, float* R, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            double l = L[i], r = R[i];
            for (auto& b : biquads) { l = b.tick (l, 0); r = b.tick (r, 1); }
            L[i] = (float) l; R[i] = (float) r;
        }
    }

    // Magnitude (dB) of the whole chain at a frequency — for the UI response curve.
    float magnitudeDb (double freq) const noexcept
    {
        double m = 1.0;
        for (auto& b : biquads) m *= b.magnitude (freq, sampleRate);
        return (float) (20.0 * std::log10 (m > 1e-9 ? m : 1e-9));
    }

private:
    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;   // a0 normalised to 1
        double z1[2] { 0, 0 }, z2[2] { 0, 0 };

        void reset() { z1[0] = z1[1] = z2[0] = z2[1] = 0.0; }

        double tick (double x, int ch) noexcept
        {
            const double y = b0 * x + z1[ch];
            z1[ch] = b1 * x - a1 * y + z2[ch];
            z2[ch] = b2 * x - a2 * y;
            return y;
        }

        void set (Type type, double fs, const Band& band)
        {
            const double f = band.freq < 20.0 ? 20.0 : (band.freq > fs * 0.49 ? fs * 0.49 : band.freq);
            const double A = std::pow (10.0, band.gainDb / 40.0);
            const double w0 = 2.0 * eqconst::kPi * f / fs;
            const double cw = std::cos (w0), sw = std::sin (w0);
            const double Q = band.q < 0.1 ? 0.1 : band.q;
            double B0, B1, B2, A0, A1, A2;

            if (type == Bell)
            {
                const double alpha = sw / (2.0 * Q);
                B0 = 1 + alpha * A;  B1 = -2 * cw;        B2 = 1 - alpha * A;
                A0 = 1 + alpha / A;  A1 = -2 * cw;        A2 = 1 - alpha / A;
            }
            else
            {
                const double alpha = sw / 2.0 * std::sqrt ((A + 1.0 / A) * (1.0 / 0.9 - 1.0) + 2.0);
                const double tsa = 2.0 * std::sqrt (A) * alpha;
                if (type == LowShelf)
                {
                    B0 =      A * ((A + 1) - (A - 1) * cw + tsa);
                    B1 =  2 * A * ((A - 1) - (A + 1) * cw);
                    B2 =      A * ((A + 1) - (A - 1) * cw - tsa);
                    A0 =          (A + 1) + (A - 1) * cw + tsa;
                    A1 =     -2 * ((A - 1) + (A + 1) * cw);
                    A2 =          (A + 1) + (A - 1) * cw - tsa;
                }
                else // HighShelf
                {
                    B0 =      A * ((A + 1) + (A - 1) * cw + tsa);
                    B1 = -2 * A * ((A - 1) + (A + 1) * cw);
                    B2 =      A * ((A + 1) + (A - 1) * cw - tsa);
                    A0 =          (A + 1) - (A - 1) * cw + tsa;
                    A1 =      2 * ((A - 1) - (A + 1) * cw);
                    A2 =          (A + 1) - (A - 1) * cw - tsa;
                }
            }
            b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0;
        }

        // |H(e^jw)| for the response curve.
        double magnitude (double freq, double fs) const noexcept
        {
            const double w = 2.0 * eqconst::kPi * freq / fs;
            const double cw = std::cos (w), c2w = std::cos (2 * w);
            const double sw = std::sin (w), s2w = std::sin (2 * w);
            const double numRe = b0 + b1 * cw + b2 * c2w, numIm = -(b1 * sw + b2 * s2w);
            const double denRe = 1.0 + a1 * cw + a2 * c2w, denIm = -(a1 * sw + a2 * s2w);
            const double num = std::sqrt (numRe * numRe + numIm * numIm);
            const double den = std::sqrt (denRe * denRe + denIm * denIm);
            return den > 1e-12 ? num / den : 1.0;
        }
    };

    std::array<Biquad, 4> biquads;
    double sampleRate = 48000.0;
};
