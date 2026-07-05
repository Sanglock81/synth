#pragma once
// ============================================================================
// Shared, JUCE-free test helpers: FFT, windowing, spectral analysis, simple
// metrics, and a minimal float32 WAV reader/writer for the golden render.
// Standard library only — keep it that way.
// ============================================================================
#include <vector>
#include <complex>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <algorithm>

namespace tu
{
    constexpr double kPi    = 3.14159265358979323846;
    constexpr double kTwoPi = 6.28318530717958647692;

    // ---- metrics -----------------------------------------------------------
    inline float peak (const std::vector<float>& x)
    {
        float p = 0.0f;
        for (float s : x) p = std::max (p, std::abs (s));
        return p;
    }

    inline double rms (const std::vector<float>& x)
    {
        double acc = 0.0;
        for (float s : x) acc += double (s) * double (s);
        return x.empty() ? 0.0 : std::sqrt (acc / double (x.size()));
    }

    // Largest absolute jump between consecutive samples.
    inline float maxDelta (const std::vector<float>& x)
    {
        float d = 0.0f;
        for (std::size_t i = 1; i < x.size(); ++i)
            d = std::max (d, std::abs (x[i] - x[i - 1]));
        return d;
    }

    inline bool allFinite (const std::vector<float>& x)
    {
        for (float s : x) if (! std::isfinite (s)) return false;
        return true;
    }

    inline double linToDb (double lin) { return 20.0 * std::log10 (std::max (lin, 1e-30)); }

    // Fundamental frequency via zero-up-crossing interval timing (sub-sample
    // interpolated). Returns 0 if too few crossings in the window.
    inline double zeroCrossHz (const std::vector<float>& x, int start, int len, double sr)
    {
        double firstX = -1.0, lastX = -1.0; int count = 0;
        float prev = x[(std::size_t) start];
        for (int i = start + 1; i < start + len; ++i)
        {
            float v = x[(std::size_t) i];
            if (prev < 0.0f && v >= 0.0f)
            {
                double frac = double (-prev) / double (v - prev);
                double xh = double (i - 1) + frac;
                if (firstX < 0.0) firstX = xh; else lastX = xh;
                ++count;
            }
            prev = v;
        }
        if (count < 3) return 0.0;
        return (count - 1) / ((lastX - firstX) / sr);
    }

    // ---- FFT (iterative radix-2 Cooley-Tukey; n must be a power of two) -----
    inline void fft (std::vector<std::complex<double>>& a)
    {
        const std::size_t n = a.size();
        for (std::size_t i = 1, j = 0; i < n; ++i)
        {
            std::size_t bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap (a[i], a[j]);
        }
        for (std::size_t len = 2; len <= n; len <<= 1)
        {
            const double ang = -kTwoPi / double (len);
            const std::complex<double> wlen (std::cos (ang), std::sin (ang));
            for (std::size_t i = 0; i < n; i += len)
            {
                std::complex<double> w (1.0, 0.0);
                for (std::size_t k = 0; k < len / 2; ++k)
                {
                    const auto u = a[i + k];
                    const auto v = a[i + k + len / 2] * w;
                    a[i + k]             = u + v;
                    a[i + k + len / 2]   = u - v;
                    w *= wlen;
                }
            }
        }
    }

    // Hann window in place.
    inline void hann (std::vector<float>& x)
    {
        const std::size_t n = x.size();
        for (std::size_t i = 0; i < n; ++i)
            x[i] *= float (0.5 - 0.5 * std::cos (kTwoPi * double (i) / double (n - 1)));
    }

    // 4-term Blackman-Harris (~-92 dB side lobes). Needed when the quantity
    // under test (e.g. residual aliasing) is far below Hann's -31 dB leakage
    // floor, which would otherwise mask a good result.
    inline void blackmanHarris (std::vector<float>& x)
    {
        const double a0 = 0.35875, a1 = 0.48829, a2 = 0.14128, a3 = 0.01168;
        const std::size_t n = x.size();
        for (std::size_t i = 0; i < n; ++i)
        {
            const double t = double (i) / double (n - 1);
            x[i] *= float (a0 - a1 * std::cos (kTwoPi * t)
                              + a2 * std::cos (2.0 * kTwoPi * t)
                              - a3 * std::cos (3.0 * kTwoPi * t));
        }
    }

    // One-sided magnitude spectrum (linear) of a real signal, given a window
    // function. Length must be a power of two.
    inline std::vector<double> magnitudeSpectrumWin (std::vector<float> sig,
                                                     void (*window)(std::vector<float>&))
    {
        window (sig);
        std::vector<std::complex<double>> a (sig.size());
        for (std::size_t i = 0; i < sig.size(); ++i) a[i] = std::complex<double> (sig[i], 0.0);
        fft (a);
        std::vector<double> mag (sig.size() / 2 + 1);
        for (std::size_t i = 0; i < mag.size(); ++i) mag[i] = std::abs (a[i]);
        return mag;
    }

    inline std::vector<double> magnitudeSpectrum (std::vector<float> sig)
    {
        return magnitudeSpectrumWin (std::move (sig), hann);
    }

    // ---- minimal float32 WAV (format code 3, IEEE float) -------------------
    inline void writeWavF32 (const std::string& path, const std::vector<float>& mono, int sampleRate)
    {
        FILE* f = std::fopen (path.c_str(), "wb");
        if (! f) return;
        auto u32 = [&](std::uint32_t v){ std::fwrite (&v, 4, 1, f); };
        auto u16 = [&](std::uint16_t v){ std::fwrite (&v, 2, 1, f); };
        const std::uint32_t dataBytes = std::uint32_t (mono.size() * sizeof (float));
        std::fwrite ("RIFF", 1, 4, f); u32 (36 + dataBytes); std::fwrite ("WAVE", 1, 4, f);
        std::fwrite ("fmt ", 1, 4, f); u32 (16); u16 (3); u16 (1);
        u32 (std::uint32_t (sampleRate)); u32 (std::uint32_t (sampleRate) * 4); u16 (4); u16 (32);
        std::fwrite ("data", 1, 4, f); u32 (dataBytes);
        std::fwrite (mono.data(), sizeof (float), mono.size(), f);
        std::fclose (f);
    }

    // Returns empty vector on failure. Assumes the format we write above.
    inline std::vector<float> readWavF32 (const std::string& path)
    {
        FILE* f = std::fopen (path.c_str(), "rb");
        std::vector<float> out;
        if (! f) return out;
        std::fseek (f, 0, SEEK_END);
        long sz = std::ftell (f);
        std::fseek (f, 44, SEEK_SET);          // skip our fixed 44-byte header
        if (sz > 44)
        {
            out.resize (std::size_t (sz - 44) / sizeof (float));
            if (std::fread (out.data(), sizeof (float), out.size(), f) != out.size())
                out.clear();
        }
        std::fclose (f);
        return out;
    }
}
