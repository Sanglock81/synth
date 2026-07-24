#pragma once
#include <vector>
#include <complex>
#include <cmath>
#include <cstdint>
#include <algorithm>

// ============================================================================
// Mip-mapped, band-limited wavetable — the source behind the oscillator's "WT"
// wave. JUCE-free, std-only.
//
// A table is N FRAMES of L samples (L a power of two). `build()` runs on the
// message thread (allocation is fine there) and, for each frame, precomputes a
// stack of MIPS: mip m keeps only the first (L/2 >> m) harmonics (top mip ~1
// harmonic, each lower octave doubles them). It does this by FFT -> zeroing the
// bins above the mip's harmonic limit (and DC) -> IFFT, so every mip is exactly
// band-limited, alias-free reconstruction of the frame.
//
// The RUNTIME read is allocation-free and STATELESS (so one shared table serves
// every voice concurrently): the oscillator picks the mip for its pitch with
// `mipForFreq()` (so a high note reads a version with few enough harmonics not to
// alias), then `read(phase, pos, mip)` linear-interpolates within the frame and
// crossfades between the two frames bracketing the position `pos` in [0,1].
//
// The oscillator reads it inside its existing oversample+decimate path, so the
// decimation FIR mops up the little residual left above the base Nyquist.
// ============================================================================

class Wavetable
{
public:
    static constexpr int   kMaxFrames  = 16;
    static constexpr float kTargetRms  = 0.25f;   // table-level loudness normalization (see 3b for cross-table)

    bool valid() const { return numFrames > 0 && tableLen > 0; }
    int  frames() const { return numFrames; }
    int  length() const { return tableLen; }
    int  mips()   const { return numMips; }

    // Build the mip stack from raw frames (each of length L, a power of two). Message-thread only.
    // equalRms: scale the whole table to a reference RMS so switching tables doesn't jump the level.
    void build (const std::vector<std::vector<float>>& rawFrames, double sampleRate, bool equalRms = true)
    {
        sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        numFrames = std::min ((int) rawFrames.size(), kMaxFrames);
        if (numFrames <= 0) { tableLen = 0; return; }
        tableLen = (int) rawFrames[0].size();
        if (tableLen < 2 || (tableLen & (tableLen - 1)) != 0) { numFrames = 0; tableLen = 0; return; }  // need power-of-two

        // mip m keeps harmonics 1..(L/2 >> m). numMips runs down to the 1-harmonic (pure sine) mip.
        numMips = 1;
        for (int h = tableLen / 2; h > 1; h >>= 1) ++numMips;
        mipHarm.assign ((std::size_t) numMips, 0);
        for (int m = 0; m < numMips; ++m) mipHarm[(std::size_t) m] = (tableLen / 2) >> m;

        flat.assign ((std::size_t) numFrames * numMips * tableLen, 0.0f);

        std::vector<std::complex<double>> spec ((std::size_t) tableLen), work ((std::size_t) tableLen);
        for (int f = 0; f < numFrames; ++f)
        {
            for (int j = 0; j < tableLen; ++j)
                spec[(std::size_t) j] = { (double) rawFrames[(std::size_t) f][(std::size_t) j], 0.0 };
            fft (spec, false);

            for (int m = 0; m < numMips; ++m)
            {
                const int H = mipHarm[(std::size_t) m];
                work = spec;
                for (int k = 0; k < tableLen; ++k)
                {
                    const int harm = (k <= tableLen / 2) ? k : tableLen - k;   // harmonic index of this bin
                    if (harm == 0 || harm > H) work[(std::size_t) k] = { 0.0, 0.0 };   // drop DC + above the limit
                }
                fft (work, true);   // inverse (scaled by 1/L)
                float* dst = frameMip (f, m);
                for (int j = 0; j < tableLen; ++j) dst[j] = (float) work[(std::size_t) j].real();
            }
        }

        if (equalRms) normalize();
    }

    // Highest mip (most harmonics) whose top harmonic still lands below the BASE Nyquist for `f0Hz`.
    // The oscillator caches this per render chunk; passing it to read() keeps read() stateless.
    int mipForFreq (double f0Hz) const
    {
        if (numMips <= 0) return 0;
        const double maxHarm = (f0Hz > 1.0e-6) ? (sr * 0.5) / f0Hz : 1.0e9;
        int m = 0;
        while (m < numMips - 1 && (double) mipHarm[(std::size_t) m] > maxHarm) ++m;
        return m;
    }

    // One sample. phase in [0,1) is the position within the frame; pos in [0,1] selects/morphs the
    // frame; mip is from mipForFreq(). Two frame reads (each linear within the frame) crossfaded.
    float read (double phase, float pos, int mip) const
    {
        const int m = mip < 0 ? 0 : (mip >= numMips ? numMips - 1 : mip);

        const float ff = std::clamp (pos, 0.0f, 1.0f) * (float) (numFrames - 1);
        int f0 = (int) ff; if (f0 > numFrames - 1) f0 = numFrames - 1; if (f0 < 0) f0 = 0;
        const int f1 = (f0 + 1 < numFrames) ? f0 + 1 : f0;
        const float fr = ff - (float) f0;

        const double idx = phase * (double) tableLen;
        int j0 = (int) idx; if (j0 >= tableLen) j0 = tableLen - 1; if (j0 < 0) j0 = 0;
        const int j1 = (j0 + 1 < tableLen) ? j0 + 1 : 0;
        const float jf = (float) (idx - (double) j0);

        const float* a = frameMip (f0, m);
        const float* b = frameMip (f1, m);
        const float va = a[j0] * (1.0f - jf) + a[j1] * jf;
        const float vb = b[j0] * (1.0f - jf) + b[j1] * jf;
        return va * (1.0f - fr) + vb * fr;
    }

private:
    float*       frameMip (int f, int m)       { return &flat[((std::size_t) (f * numMips + m)) * (std::size_t) tableLen]; }
    const float* frameMip (int f, int m) const { return &flat[((std::size_t) (f * numMips + m)) * (std::size_t) tableLen]; }

    void normalize()
    {
        // RMS of the full-band mip across all frames -> one global scale (keeps relative frame
        // character; only removes gross level differences between tables).
        double e = 0.0; std::size_t n = 0;
        for (int f = 0; f < numFrames; ++f)
        {
            const float* t = frameMip (f, 0);
            for (int j = 0; j < tableLen; ++j) { e += (double) t[j] * t[j]; ++n; }
        }
        const double rms = (n > 0) ? std::sqrt (e / (double) n) : 0.0;
        if (rms > 1.0e-9)
        {
            const float g = (float) ((double) kTargetRms / rms);
            for (auto& v : flat) v *= g;
        }
    }

    // In-place iterative radix-2 Cooley-Tukey. Build-time only (allocation/perf not a concern).
    // forward: no scaling; inverse: divide by N. N must be a power of two.
    static void fft (std::vector<std::complex<double>>& a, bool inverse)
    {
        const int n = (int) a.size();
        for (int i = 1, j = 0; i < n; ++i)
        {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap (a[(std::size_t) i], a[(std::size_t) j]);
        }
        const double twoPi = 6.283185307179586;
        for (int len = 2; len <= n; len <<= 1)
        {
            const double ang = (inverse ? twoPi : -twoPi) / (double) len;
            const std::complex<double> wlen (std::cos (ang), std::sin (ang));
            for (int i = 0; i < n; i += len)
            {
                std::complex<double> w (1.0, 0.0);
                for (int k = 0; k < len / 2; ++k)
                {
                    const std::complex<double> u = a[(std::size_t) (i + k)];
                    const std::complex<double> v = a[(std::size_t) (i + k + len / 2)] * w;
                    a[(std::size_t) (i + k)]           = u + v;
                    a[(std::size_t) (i + k + len / 2)] = u - v;
                    w *= wlen;
                }
            }
        }
        if (inverse)
            for (auto& x : a) x /= (double) n;
    }

    double sr = 48000.0;
    int    tableLen  = 0;
    int    numFrames = 0;
    int    numMips   = 0;
    std::vector<int>   mipHarm;   // top harmonic count per mip (L/2, L/4, ... 1)
    std::vector<float> flat;      // [frame][mip][sample], row-major, length numFrames*numMips*L
};
