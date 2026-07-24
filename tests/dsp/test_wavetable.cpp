// ============================================================================
// #95 Wavetable (3a — engine). The mip-mapped band-limited table and its
// oscillator integration: alias-free high notes, click-free position morph, and
// the standing golden guard (WT is not the default wave). JUCE-free (DSP only).
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "Wavetable.h"
#include "SynthVoice.h"
#include "test_util.h"
#include <vector>
#include <cmath>
#include <complex>

namespace
{
    constexpr double kSR = 48000.0;
    constexpr int    kWtWave = 4;    // Wave::Wavetable

    // A rich 2-frame table: frame0 a band-limited saw (harmonics 1/k), frame1 an odd-harmonic square.
    Wavetable richTable (int L = 256)
    {
        std::vector<std::vector<float>> f (2, std::vector<float> ((std::size_t) L, 0.0f));
        for (int n = 0; n < L; ++n)
        {
            double saw = 0.0, sqr = 0.0;
            for (int k = 1; k <= L / 2; ++k)
            {
                const double s = std::sin (tu::kTwoPi * k * n / L) / k;
                saw += s; if (k % 2) sqr += s;
            }
            f[0][(std::size_t) n] = (float) saw;
            f[1][(std::size_t) n] = (float) sqr;
        }
        Wavetable wt; wt.build (f, kSR, true); return wt;
    }

    // Fraction of spectral energy NOT within +/-8 bins of a harmonic of f0 (i.e. aliasing).
    double aliasFraction (const std::vector<float>& x, double f0, int fftN = 8192)
    {
        std::vector<float> w = tu::slice (x, x.size() / 2, (std::size_t) fftN);
        tu::blackmanHarris (w);
        std::vector<std::complex<double>> a ((std::size_t) fftN);
        for (int i = 0; i < fftN; ++i) a[(std::size_t) i] = { (double) w[(std::size_t) i], 0.0 };
        tu::fft (a);
        std::vector<int> hb; for (int k = 1; k * f0 < kSR * 0.5; ++k) hb.push_back ((int) std::lround (f0 * k * fftN / kSR));
        auto nearH = [&] (int b) { for (int h : hb) if (std::abs (b - h) <= 8) return true; return false; };
        double harm = 0.0, alias = 0.0;
        for (int b = 2; b < fftN / 2; ++b) { const double e = std::norm (a[(std::size_t) b]); (nearH (b) ? harm : alias) += e; }
        return (harm + alias) > 0.0 ? alias / (harm + alias) : 0.0;
    }

    VoiceParams wtPatch (const Wavetable* table, float pos)
    {
        VoiceParams p;
        p.osc1Level = 1.0f; p.osc2Level = 0.0f; p.osc3Level = 0.0f;
        p.osc1Wave = kWtWave; p.osc1WtTable = table; p.osc1WtPos = pos;
        p.ampA = 0.001f; p.ampD = 0.0f; p.ampS = 1.0f; p.ampR = 0.005f;
        p.cutoffHz = 20000.0f; p.resonance = 0.0f; p.filterEnvAmt = 0.0f; p.drive = 0.0f;
        return p;
    }

    std::vector<float> playNote (const VoiceParams& p, int note, int n)
    {
        SynthVoice v; v.prepare (kSR);
        std::vector<float> buf ((std::size_t) n, 0.0f);
        v.noteOn (note, 1.0f, 1, 0, 0, false, p.osc1Phase, p.osc2Phase, p.osc3Phase);
        v.render (buf.data(), n, p);
        return buf;
    }
}

TEST_CASE ("Wavetable: mips band-limit so a high note does not alias, and a low note reconstructs", "[dsp][wavetable]")
{
    const int L = 256;
    Wavetable wt = richTable (L);
    REQUIRE (wt.valid());
    REQUIRE (wt.frames() == 2);
    REQUIRE (wt.length() == L);

    SECTION ("mip index rises (fewer harmonics) with pitch")
    {
        REQUIRE (wt.mipForFreq (55.0)   <= wt.mipForFreq (440.0));
        REQUIRE (wt.mipForFreq (440.0)  <= wt.mipForFreq (4186.0));
        REQUIRE (wt.mipForFreq (55.0)   == 0);            // low note gets the full-band mip
    }

    SECTION ("low note reconstructs a saw (1/k harmonics -> h1:h2 ~ 2:1)")
    {
        const double f0 = 110.0; const int mip = wt.mipForFreq (f0);
        std::vector<float> buf (8192); double ph = 0.0;
        for (auto& s : buf) { s = wt.read (ph, 0.0f, mip); ph += f0 / kSR; if (ph >= 1.0) ph -= 1.0; }
        auto mag = [&] (double hz) { double re = 0, im = 0; int N = (int) buf.size();
            for (int n = 0; n < N; ++n) { const double a = tu::kTwoPi * hz * n / kSR; re += buf[(std::size_t) n] * std::cos (a); im -= buf[(std::size_t) n] * std::sin (a); }
            return std::sqrt (re * re + im * im) / N; };
        REQUIRE (mag (220.0) > 1e-6);
        REQUIRE (mag (110.0) / mag (220.0) == Catch::Approx (2.0).margin (0.25));
    }

    SECTION ("high notes stay alias-free across the whole position sweep")
    {
        for (double f0 : { 2093.0, 4186.0 })            // C7, C8
            for (float pos : { 0.0f, 0.5f, 1.0f })
            {
                const int mip = wt.mipForFreq (f0);
                std::vector<float> buf (16384); double ph = 0.0;
                for (auto& s : buf) { s = wt.read (ph, pos, mip); ph += f0 / kSR; if (ph >= 1.0) ph -= 1.0; }
                INFO ("f0=" << f0 << " pos=" << pos);
                REQUIRE (tu::allFinite (buf));
                REQUIRE (aliasFraction (buf, f0) < 0.02);
            }
    }
}

TEST_CASE ("Wavetable osc: a WT voice plays a clean high note through the full voice path", "[dsp][wavetable][voice]")
{
    Wavetable wt = richTable();
    auto out = playNote (wtPatch (&wt, 0.0f), 108, 16384);   // C8
    REQUIRE (tu::allFinite (out));
    REQUIRE (tu::peak (out) > 0.01f);                        // actually sounding
    const double f0 = 440.0 * std::pow (2.0, (108 - 69) / 12.0);
    INFO ("C8 f0=" << f0 << " aliasFrac=" << aliasFraction (out, f0));
    REQUIRE (aliasFraction (out, f0) < 0.05);                // mip + 4x/decimate keep it clean
}

TEST_CASE ("Wavetable osc: sweeping WT POS is click-free (zipper-safe smoothing)", "[dsp][wavetable][voice]")
{
    Wavetable wt = richTable();
    const int N = 8192, chunk = 32;

    // Reference: a steady WT note (position held) — its natural per-sample delta ceiling.
    SynthVoice steady; steady.prepare (kSR);
    auto pS = wtPatch (&wt, 0.5f);
    std::vector<float> ref (N, 0.0f);
    steady.noteOn (60, 1.0f, 1, 0, 0, false, 0, 0, 0);
    for (int i = 0; i < N; i += chunk) steady.render (ref.data() + i, chunk, pS);

    // Swept: move the position 0 -> 1 across the note, one step per chunk.
    SynthVoice swept; swept.prepare (kSR);
    std::vector<float> buf (N, 0.0f);
    swept.noteOn (60, 1.0f, 1, 0, 0, false, 0, 0, 0);
    for (int i = 0; i < N; i += chunk)
    {
        auto p = wtPatch (&wt, (float) i / (float) N);
        swept.render (buf.data() + i, chunk, p);
    }
    REQUIRE (tu::allFinite (buf));
    REQUIRE (tu::maxDelta (buf) < tu::maxDelta (ref) * 1.5f + 1.0e-3f);   // no zipper step from the sweep
}

TEST_CASE ("Wavetable osc: WT is inert unless selected (non-WT patch bit-identical)", "[dsp][wavetable][golden]")
{
    // A saw voice renders identically whether or not a WT table happens to be attached — the WT
    // fields must not leak into a non-WT patch (the standing golden guarantee at default settings).
    Wavetable wt = richTable();
    VoiceParams a; a.osc1Wave = 0; a.osc1Level = 1.0f; a.osc2Level = 0.0f; a.osc3Level = 0.0f;
    VoiceParams b = a; b.osc1WtTable = &wt; b.osc1WtPos = 0.7f;   // attached but wave is Saw
    auto ra = playNote (a, 60, 2048);
    auto rb = playNote (b, 60, 2048);
    REQUIRE (ra == rb);
}
