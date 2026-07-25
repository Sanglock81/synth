// FP CONTRACTION OFF for the whole translation unit: no compiler may fuse a mul-add
// into an FMA here, so every +,-,*,/ is strict IEEE and the generated table bytes are
// bit-identical on GCC (Linux) and MSVC (Windows). See WavetableGen.h.
#pragma STDC FP_CONTRACT OFF

#include "WavetableGen.h"
#include <vector>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <utility>

namespace wtgen
{
namespace
{
    constexpr double kTwoPi  = 6.283185307179586;
    constexpr double kPi     = 3.141592653589793;
    constexpr double kHalfPi = 1.5707963267948966;

    // Deterministic sine on all platforms: range-reduce with exact subtractions, then a degree-13
    // odd polynomial (Taylor) on [0, pi/2] (error < 1e-9). No std::sin (which varies across libms).
    double detSin (double x)
    {
        while (x >  kPi) x -= kTwoPi;
        while (x < -kPi) x += kTwoPi;
        double sign = 1.0;
        if (x < 0.0) { x = -x; sign = -1.0; }        // sin(-x) = -sin(x);  x now in [0, pi]
        if (x > kHalfPi) x = kPi - x;                // sin(pi-x) = sin(x); x now in [0, pi/2]
        const double x2 = x * x;
        const double p  = x * (1.0 + x2 * (-1.0 / 6.0 + x2 * (1.0 / 120.0 + x2 * (-1.0 / 5040.0
                          + x2 * (1.0 / 362880.0 + x2 * (-1.0 / 39916800.0 + x2 * (1.0 / 6227020800.0)))))));
        return sign * p;
    }

    // Fixed 32-bit xorshift PRNG (we own it — never a std distribution, which forks across stdlibs).
    struct Rng
    {
        std::uint32_t s;
        explicit Rng (std::uint32_t seed) : s (seed ? seed : 0x9e3779b9u) {}
        std::uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
        float nextFloat() { return (float) (next() >> 8) * (1.0f / 16777216.0f); }   // [0,1), 24-bit
        int   nextInt (int n) { return (int) (next() % (std::uint32_t) n); }
    };

    // One harmonic of a frame: index k (1..L/2), amplitude, integer phase step in [0, L).
    struct Harmonic { int k; float amp; int phase; };
    using FrameSpec = std::vector<Harmonic>;
    using TableSpec = std::vector<FrameSpec>;   // N frames

    int mipCount (int L)
    {
        int n = 1;
        for (int h = L / 2; h > 1; h >>= 1) ++n;   // L/2, L/4, ... 1
        return n;
    }

    // Additive synthesis -> band-limited mips -> equal-RMS normalize -> Wavetable. The SINGLE shared
    // build path for factory tables AND the randomizer, so their loudness is calibrated identically.
    Wavetable buildFromHarmonics (const TableSpec& spec, int L, double sr, float targetRms = 0.25f)
    {
        const int nFrames = std::min ((int) spec.size(), Wavetable::kMaxFrames);
        const int nMips   = mipCount (L);
        std::vector<int> mipHarm ((std::size_t) nMips);
        for (int m = 0; m < nMips; ++m) mipHarm[(std::size_t) m] = (L / 2) >> m;

        std::vector<double> sine ((std::size_t) L);            // sin(2*pi*i/L), deterministic
        for (int i = 0; i < L; ++i) sine[(std::size_t) i] = detSin (kTwoPi * (double) i / (double) L);

        std::vector<float> flat ((std::size_t) nFrames * nMips * L, 0.0f);
        for (int f = 0; f < nFrames; ++f)
        {
            const FrameSpec& fs = spec[(std::size_t) f];
            for (int m = 0; m < nMips; ++m)
            {
                const int H = mipHarm[(std::size_t) m];
                float* dst = &flat[((std::size_t) (f * nMips + m)) * (std::size_t) L];
                for (int n = 0; n < L; ++n)
                {
                    double acc = 0.0;
                    for (const Harmonic& h : fs)
                    {
                        if (h.k < 1 || h.k > H) continue;      // band-limit to this mip's harmonic ceiling
                        const int idx = (h.k * n + h.phase) % L;   // exact integer phase index
                        acc += (double) h.amp * sine[(std::size_t) idx];
                    }
                    dst[n] = (float) acc;
                }
            }
        }

        // Equal-RMS: one global scale from the full-band mip so every table lands at the same level.
        double e = 0.0; std::size_t cnt = 0;
        for (int f = 0; f < nFrames; ++f)
        {
            const float* t = &flat[((std::size_t) (f * nMips)) * (std::size_t) L];
            for (int n = 0; n < L; ++n) { e += (double) t[n] * (double) t[n]; ++cnt; }
        }
        const double rms = (cnt > 0) ? std::sqrt (e / (double) cnt) : 0.0;
        if (rms > 1.0e-9)
        {
            const float g = (float) ((double) targetRms / rms);
            for (auto& v : flat) v = v * g;
        }

        Wavetable wt;
        wt.adopt (std::move (flat), L, nFrames, nMips, std::move (mipHarm), sr);
        return wt;
    }

    // ---- factory specs (procedural, all deterministic: rational shaping only, no transcendentals) --

    // A rational "brightness" rolloff: tilt 0 keeps highs, tilt 1 rolls them off. No pow/exp.
    float tiltGain (int k, float tilt) { return 1.0f / (1.0f + tilt * (float) k * 0.14f); }

    // Lorentzian formant bump (deterministic; replaces a Gaussian, which needs exp).
    float formant (int k, float centre, float width)
    {
        const float d = (float) k - centre;
        return (width * width) / (width * width + d * d);
    }

    TableSpec specAnalog (int L, int nFrames)     // saw -> square: fade the EVEN harmonics out
    {
        TableSpec s ((std::size_t) nFrames);
        for (int f = 0; f < nFrames; ++f)
        {
            const float t = (nFrames > 1) ? (float) f / (float) (nFrames - 1) : 0.0f;
            for (int k = 1; k <= L / 2; ++k)
            {
                const float even = (k % 2 == 0) ? (1.0f - t) : 1.0f;   // even harmonics fade to 0 by the last frame
                s[(std::size_t) f].push_back ({ k, even / (float) k, 0 });
            }
        }
        return s;
    }

    TableSpec specSweep (int L, int nFrames)      // harmonic sweep: fundamental -> full spectrum
    {
        TableSpec s ((std::size_t) nFrames);
        for (int f = 0; f < nFrames; ++f)
        {
            const float t = (nFrames > 1) ? (float) f / (float) (nFrames - 1) : 0.0f;
            const int   top = 1 + (int) (t * (float) (L / 2 - 1));   // how many harmonics are present
            for (int k = 1; k <= top; ++k) s[(std::size_t) f].push_back ({ k, 1.0f / (float) k, 0 });
        }
        return s;
    }

    TableSpec specVowel (int L)                    // a -> e -> i -> o -> u (2 Lorentzian formants each)
    {
        struct V { float f1, f2; };
        const V vowels[] { { 5.0f, 8.0f }, { 3.0f, 12.0f }, { 2.0f, 15.0f }, { 3.0f, 6.0f }, { 2.0f, 5.0f } };
        TableSpec s (5);
        for (int f = 0; f < 5; ++f)
            for (int k = 1; k <= L / 2; ++k)
            {
                const float a = (formant (k, vowels[f].f1, 1.5f) + 0.7f * formant (k, vowels[f].f2, 2.0f)) / (float) k;
                if (a > 1.0e-4f) s[(std::size_t) f].push_back ({ k, a, 0 });
            }
        return s;
    }

    TableSpec specDigital (int L, int nFrames)     // bright/harsh: emphasise odd + upper harmonics
    {
        TableSpec s ((std::size_t) nFrames);
        for (int f = 0; f < nFrames; ++f)
        {
            const float t = (nFrames > 1) ? (float) f / (float) (nFrames - 1) : 0.0f;   // 0 dark .. 1 bright
            for (int k = 1; k <= L / 2; ++k)
            {
                const float odd = (k % 2 == 1) ? 1.0f : 0.45f;
                const float amp = odd * tiltGain (k, 1.0f - t) / (float) k * (0.5f + 0.5f * (float) k / (float) (L / 2));
                s[(std::size_t) f].push_back ({ k, amp, 0 });
            }
        }
        return s;
    }
}

int factoryCount() { return 4; }

const char* factoryName (int id)
{
    switch (id) { case 0: return "Analog"; case 1: return "Sweep"; case 2: return "Vowel"; case 3: return "Digital"; }
    return "Analog";
}

Wavetable buildFactory (int id, double sampleRate)
{
    const int L = kFrameLen;
    switch (id)
    {
        case 1:  return buildFromHarmonics (specSweep  (L, 8), L, sampleRate);
        case 2:  return buildFromHarmonics (specVowel  (L),    L, sampleRate);
        case 3:  return buildFromHarmonics (specDigital(L, 6), L, sampleRate);
        default: return buildFromHarmonics (specAnalog (L, 8), L, sampleRate);
    }
}

Wavetable buildRandom (std::uint32_t seed, double sampleRate)
{
    const int L = kFrameLen;
    const int nFrames = 4;
    const int K = 24;                              // bounded harmonic count (band-limited by construction)
    Rng rng (seed);

    // One random character (amps + phases), morphed across frames by a rational brightness tilt.
    std::vector<float> baseAmp ((std::size_t) (K + 1), 0.0f);
    std::vector<int>   basePhase ((std::size_t) (K + 1), 0);
    for (int k = 1; k <= K; ++k)
    {
        baseAmp[(std::size_t) k]   = (0.3f + 0.7f * rng.nextFloat()) / (float) k;   // 1/k-ish, randomized
        basePhase[(std::size_t) k] = rng.nextInt (L);
    }

    TableSpec spec ((std::size_t) nFrames);
    for (int f = 0; f < nFrames; ++f)
    {
        const float tilt = (nFrames > 1) ? (float) f / (float) (nFrames - 1) : 0.0f;   // 0 bright .. 1 dark
        for (int k = 1; k <= K; ++k)
            spec[(std::size_t) f].push_back ({ k, baseAmp[(std::size_t) k] * tiltGain (k, tilt), basePhase[(std::size_t) k] });
    }
    return buildFromHarmonics (spec, L, sampleRate);
}

}   // namespace wtgen
