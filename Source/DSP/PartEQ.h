#pragma once
#include <cmath>
#include <array>

// M_PI isn't standard; Source/DSP/ is dependency-free.
namespace peqconst { inline constexpr double kPi = 3.14159265358979323846; }

// ============================================================================
// Per-part parametric EQ — JUCE-free (standard library only). FOUR fully parametric
// RBJ peaking (bell) bands in series, each with its own freq / gain / Q and an on/off
// switch. Stereo (independent state per channel), transposed-direct-form-II so
// coefficient updates don't click. K1: this is the single consolidated per-part EQ,
// applied as a FIXED final stage at the end of each part's chain (post-FX). A band
// that is off is compiled to unity (gainDb = 0), so toggling it is click-free and its
// magnitude contribution is exactly 1. With every band at 0 dB (or off) the whole EQ
// is unity, and the chain skips it when the section is disabled, so it stays
// bit-identical until used.
// ============================================================================

class PartEQ
{
public:
    static constexpr int kNumBands = 4;
    struct Band { float freq = 1000.0f, gainDb = 0.0f, q = 0.9f; bool on = true; };

    void prepare (double sr) { sampleRate = sr > 0.0 ? sr : 48000.0; reset(); }
    void reset() { for (auto& b : bands) b.reset(); }

    void setBands (const Band& b1, const Band& b2, const Band& b3, const Band& b4)
    {
        bands[0].set (sampleRate, b1);
        bands[1].set (sampleRate, b2);
        bands[2].set (sampleRate, b3);
        bands[3].set (sampleRate, b4);
    }

    void process (float* L, float* R, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            double l = L[i], r = R[i];
            for (auto& b : bands) { l = b.tick (l, 0); r = b.tick (r, 1); }
            L[i] = (float) l; R[i] = (float) r;
        }
    }

    // Adopt another instance's filter state (for the FX reorder crossfade).
    void copyStateFrom (const PartEQ& o) { bands = o.bands; }

    // Magnitude (dB) of the whole 4-band chain at a frequency — for a UI response curve.
    float magnitudeDb (double freq) const noexcept
    {
        double m = 1.0;
        for (auto& b : bands) m *= b.magnitude (freq, sampleRate);
        return (float) (20.0 * std::log10 (m > 1e-9 ? m : 1e-9));
    }

private:
    struct Bell
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

        void set (double fs, const Band& band)
        {
            const double f = band.freq < 20.0 ? 20.0 : (band.freq > fs * 0.49 ? fs * 0.49 : band.freq);
            const double gainDb = band.on ? band.gainDb : 0.0;   // off ⇒ unity, click-free
            const double A = std::pow (10.0, gainDb / 40.0);
            const double w0 = 2.0 * peqconst::kPi * f / fs;
            const double cw = std::cos (w0), sw = std::sin (w0);
            const double Q = band.q < 0.1 ? 0.1 : band.q;
            const double alpha = sw / (2.0 * Q);
            const double B0 = 1 + alpha * A, B1 = -2 * cw,       B2 = 1 - alpha * A;
            const double A0 = 1 + alpha / A, A1 = -2 * cw,       A2 = 1 - alpha / A;
            b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0;
        }

        double magnitude (double freq, double fs) const noexcept
        {
            const double w = 2.0 * peqconst::kPi * freq / fs;
            const double cw = std::cos (w), c2w = std::cos (2 * w);
            const double sw = std::sin (w), s2w = std::sin (2 * w);
            const double numRe = b0 + b1 * cw + b2 * c2w, numIm = -(b1 * sw + b2 * s2w);
            const double denRe = 1.0 + a1 * cw + a2 * c2w, denIm = -(a1 * sw + a2 * s2w);
            const double num = std::sqrt (numRe * numRe + numIm * numIm);
            const double den = std::sqrt (denRe * denRe + denIm * denIm);
            return den > 1e-12 ? num / den : 1.0;
        }
    };

    std::array<Bell, kNumBands> bands;
    double sampleRate = 48000.0;
};
