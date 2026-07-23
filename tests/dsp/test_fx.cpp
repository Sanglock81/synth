// ============================================================================
// [6b] Hand-rolled stereo FX: chorus, delay, reverb, width, and the reorderable
// FXChain (with its click-free crossfade). JUCE-free and deterministic — these
// exercise the DSP directly, the plugin layer covers the APVTS/order-property path.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "Chorus.h"
#include "StereoDelay.h"
#include "Reverb.h"
#include "StereoWidth.h"
#include "FXChain.h"
#include "test_util.h"
#include "alloc_hook.h"
#include <vector>
#include <cmath>

namespace
{
    constexpr double kSR = 48000.0;
    constexpr int    kBlock = 128;

    // A mono-duplicated sine (L == R), as the synth feeds the FX chain.
    void fillMonoSine (std::vector<float>& L, std::vector<float>& R, double hz, float amp = 0.5f)
    {
        for (std::size_t i = 0; i < L.size(); ++i)
        {
            const float s = amp * (float) std::sin (tu::kTwoPi * hz * double (i) / kSR);
            L[i] = s; R[i] = s;
        }
    }

    double rmsDiff (const std::vector<float>& a, const std::vector<float>& b)
    {
        double acc = 0.0;
        for (std::size_t i = 0; i < a.size(); ++i) { const double d = a[i] - b[i]; acc += d * d; }
        return std::sqrt (acc / (double) a.size());
    }
}

// ---------------------------------------------------------------------------
TEST_CASE ("StereoWidth: mono collapse, identity, and widening", "[6b][fx][width]")
{
    const int N = 4096;
    std::vector<float> L (N), R (N);
    // A genuinely stereo source: L and R are different sines.
    for (int i = 0; i < N; ++i)
    {
        L[(std::size_t) i] = 0.5f * (float) std::sin (tu::kTwoPi * 220.0 * i / kSR);
        R[(std::size_t) i] = 0.5f * (float) std::sin (tu::kTwoPi * 330.0 * i / kSR);
    }

    auto run = [&] (float w)
    {
        StereoWidth fx; fx.prepare (kSR); fx.setWidth (w); fx.reset();
        auto l = L, r = R;
        fx.process (l.data(), r.data(), N);
        return std::pair<std::vector<float>, std::vector<float>> { l, r };
    };

    auto [l1, r1] = run (1.0f);      // identity
    REQUIRE (rmsDiff (l1, L) < 1e-6);
    REQUIRE (rmsDiff (r1, R) < 1e-6);

    auto [l0, r0] = run (0.0f);      // mono: L == R
    REQUIRE (rmsDiff (l0, r0) < 1e-6);

    auto [l2, r2] = run (2.0f);      // wide: more L/R difference than the source
    REQUIRE (rmsDiff (l2, r2) > rmsDiff (L, R));
}

// ---------------------------------------------------------------------------
// width > 1 synthesizes side content from the MID via an allpass cascade, so a DRY
// MONO source audibly widens — and folds back to mono cleanly (no comb notches),
// because the synthesized content is purely antisymmetric (L += d, R -= d).
namespace
{
    double correlation (const std::vector<float>& a, const std::vector<float>& b)
    {
        double sa = 0, sb = 0, sab = 0;
        for (std::size_t i = 0; i < a.size(); ++i) { sa += a[i]*a[i]; sb += b[i]*b[i]; sab += a[i]*b[i]; }
        return (sa > 1e-12 && sb > 1e-12) ? sab / std::sqrt (sa * sb) : 1.0;
    }
}

// FX SAT: a variable-threshold soft/hard clipper (velocity-sensitive), 2x-oversampled.
TEST_CASE ("StereoWidth SAT: threshold clipper — velocity-sensitive, level-neutral, warm, clean", "[6b][fx][width][sat]")
{
    const int N = 16384;

    auto magAt = [] (const std::vector<float>& x, std::size_t start, int fftN, double sr, double hz)
    {
        std::vector<float> w = tu::slice (x, start, (std::size_t) fftN);
        tu::blackmanHarris (w);
        std::vector<std::complex<double>> a ((std::size_t) fftN);
        for (int i = 0; i < fftN; ++i) a[(std::size_t) i] = { (double) w[(std::size_t) i], 0.0 };
        tu::fft (a);
        const int bin = std::clamp ((int) std::lround (hz * fftN / sr), 0, fftN / 2);
        return std::abs (a[(std::size_t) bin]);
    };
    // Render a mono sine of amplitude `amp` at `hz` through SAT (width=1: shaper alone).
    auto render = [&] (float sat, double hz, float amp)
    {
        std::vector<float> L (N), R (N); fillMonoSine (L, R, hz, amp);
        StereoWidth fx; fx.prepare (kSR); fx.setWidth (1.0f); fx.setSat (sat); fx.reset();
        for (int i = 0; i < N; i += kBlock) fx.process (L.data()+i, R.data()+i, kBlock);
        return L;
    };
    auto thd = [&] (const std::vector<float>& x, double f0)
    {
        const std::size_t s = (std::size_t) (N / 2);
        const double f = magAt (x, s, 8192, kSR, f0);
        double h = 0.0; for (int k = 2; k <= 10; ++k) { const double m = magAt (x, s, 8192, kSR, f0 * k); h += m * m; }
        return f > 1e-9 ? std::sqrt (h) / f : 0.0;
    };

    SECTION ("sat=0 is bit-identical to the no-sat path")
    {
        std::vector<float> L (N), R (N); fillMonoSine (L, R, 220.0);
        std::vector<float> la = L, ra = R, lb = L, rb = R;
        StereoWidth a; a.prepare (kSR); a.setWidth (1.5f); a.setSat (0.0f); a.reset();
        StereoWidth b; b.prepare (kSR); b.setWidth (1.5f);                 b.reset();
        for (int i = 0; i < N; i += kBlock) { a.process (la.data()+i, ra.data()+i, kBlock); b.process (lb.data()+i, rb.data()+i, kBlock); }
        REQUIRE (la == lb); REQUIRE (ra == rb);
    }

    SECTION ("velocity-sensitive: at one SAT, a quiet note stays clean but a loud note clips")
    {
        // Realistic per-part levels: a soft (low-velocity) note ~0.12, a hard note ~0.5.
        const double quietThd = thd (render (0.5f, 300.0, 0.12f), 300.0);   // below threshold -> ~clean
        const double loudThd  = thd (render (0.5f, 300.0, 0.50f), 300.0);   // above threshold -> clips
        INFO ("THD @ sat=0.5  quiet(0.12)=" << quietThd << "  loud(0.50)=" << loudThd);
        REQUIRE (loudThd > quietThd * 3.0);   // the CORE of the spec: harder playing -> more distortion
        REQUIRE (quietThd < 0.10);            // a quiet note is barely touched at a mid setting
    }

    SECTION ("moderate SAT gives the tube even-harmonic colour, DC-safe")
    {
        const auto L = render (0.5f, 300.0, 0.6f);
        REQUIRE (tu::allFinite (L));
        const std::size_t s = (std::size_t) (N / 2);
        const double fund = magAt (L, s, 8192, kSR, 300.0);
        const double h2   = magAt (L, s, 8192, kSR, 600.0);
        const double h3   = magAt (L, s, 8192, kSR, 900.0);
        const double dc   = magAt (L, s, 8192, kSR, 0.0);
        INFO ("fund=" << fund << " h2=" << h2 << " h3=" << h3 << " dc=" << dc);
        REQUIRE (h2 > 0.08 * fund);   // a real even harmonic (tube warmth)
        REQUIRE (dc < 0.01 * fund);   // DC blocker holds
    }

    SECTION ("full SAT is a hard clip (sine -> square, rich harmonics)")
    {
        INFO ("THD  clean=" << thd (render (0.0f, 300.0, 0.6f), 300.0) << "  full=" << thd (render (1.0f, 300.0, 0.6f), 300.0));
        REQUIRE (thd (render (0.0f, 300.0, 0.6f), 300.0) < 0.01);
        REQUIRE (thd (render (1.0f, 300.0, 0.6f), 300.0) > 0.35);
    }

    SECTION ("two-stage: the top half is edgier — more high-order harmonics than the smoothed low half")
    {
        // Same drive, same fundamental. The bottom half is a SOFT knee rounded by the smoothing
        // lowpass (high harmonics roll off); the top half hardens toward a square AND opens the
        // lowpass (high harmonics survive). So the high-order/low-order energy ratio must climb.
        auto hiLoRatio = [&] (float sat)
        {
            const auto L = render (sat, 300.0, 0.6f);
            const std::size_t s = (std::size_t) (N / 2);
            double lo = 0.0, hi = 0.0;
            for (int k = 2; k <= 4;  ++k) { const double m = magAt (L, s, 8192, kSR, 300.0 * k); lo += m * m; }
            for (int k = 7; k <= 15; ++k) { const double m = magAt (L, s, 8192, kSR, 300.0 * k); hi += m * m; }
            return std::sqrt (hi) / (std::sqrt (lo) + 1e-9);
        };
        const double soft = hiLoRatio (0.4f);   // soft + smoothed -> HF rolled off
        const double hard = hiLoRatio (0.9f);   // hard  + open     -> edgy, HF-rich
        INFO ("hi/lo harmonic ratio  soft(0.4)=" << soft << "  hard(0.9)=" << hard);
        REQUIRE (hard > soft * 1.5);            // the top half is audibly harder/edgier than the bottom
    }

    SECTION ("loudness stays ~flat across the sweep (SAT is tonal, not a volume knob)")
    {
        // A steady tone at a realistic level; RMS should not swing much as SAT rises.
        double lo = 1e9, hi = 0.0;
        for (float sat : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
        {
            const double r = tu::rms (tu::slice (render (sat, 300.0, 0.5f), (std::size_t) (N/2), (std::size_t) (N/2)));
            INFO ("sat=" << sat << " rms=" << r);
            lo = std::min (lo, r); hi = std::max (hi, r);
        }
        REQUIRE (hi / lo < 1.6);   // within ~4 dB across the whole sweep (level-neutral)
    }

    SECTION ("makeup is never a hidden boost: output peak never exceeds input peak")
    {
        // Real-ish material: a two-tone chord at a healthy level, held at each SAT setting long
        // enough for the auto-makeup to settle (a real knob turn is slow vs the ~30 ms follower).
        std::vector<float> L (N), R (N);
        for (int i = 0; i < N; ++i)
        { const float v = 0.35f * (float) (std::sin (tu::kTwoPi * 220.0 * i / kSR) + std::sin (tu::kTwoPi * 330.0 * i / kSR));
          L[(std::size_t) i] = v; R[(std::size_t) i] = v; }
        const float inPeak = tu::peak (L);
        for (float sat : { 0.25f, 0.5f, 0.75f, 1.0f })
        {
            auto l = L, r = R;
            StereoWidth fx; fx.prepare (kSR); fx.setWidth (1.0f); fx.setSat (sat); fx.reset();
            for (int i = 0; i < N; i += kBlock) fx.process (l.data()+i, r.data()+i, kBlock);
            const float outPeak = tu::peak (tu::slice (l, (std::size_t) (N/2), (std::size_t) (N/2)));   // settled
            INFO ("sat=" << sat << " in peak=" << inPeak << " out peak=" << outPeak);
            REQUIRE (outPeak < inPeak * 1.05f);   // clipping only reduces the peak; makeup never boosts past input
        }
    }

    SECTION ("2x oversampling keeps a hard-clipped HIGH note clean (low aliasing)")
    {
        const double f0 = 5000.0;                               // worst case: high note + hard knee
        const int    fftN = 8192;
        const auto   L = render (1.0f, f0, 0.5f);
        std::vector<float> w = tu::slice (L, (std::size_t) (N/2), (std::size_t) fftN);
        tu::blackmanHarris (w);
        std::vector<std::complex<double>> a ((std::size_t) fftN);
        for (int i = 0; i < fftN; ++i) a[(std::size_t) i] = { (double) w[(std::size_t) i], 0.0 };
        tu::fft (a);
        // Harmonic bins of 5 kHz below Nyquist (5/10/15/20 k); a Blackman-Harris line spreads ~8 bins.
        std::vector<int> harmBin; for (int k = 1; k * f0 < kSR * 0.5; ++k) harmBin.push_back ((int) std::lround (f0 * k * fftN / kSR));
        auto nearHarm = [&] (int b) { for (int hb : harmBin) if (std::abs (b - hb) <= 8) return true; return false; };
        double harm = 0.0, alias = 0.0;
        for (int b = 2; b < fftN / 2; ++b) { const double e = std::norm (a[(std::size_t) b]); (nearHarm (b) ? harm : alias) += e; }
        const double aliasFrac = (harm + alias) > 0 ? alias / (harm + alias) : 0.0;
        INFO ("alias energy fraction = " << aliasFrac);
        REQUIRE (tu::allFinite (L));
        REQUIRE (aliasFrac < 0.05);   // 2x + half-band keeps folded aliases well below the real harmonics
    }

    SECTION ("sweeping SAT live (incl. the engage across zero) is click-free")
    {
        std::vector<float> L (N), R (N); fillMonoSine (L, R, 220.0);
        std::vector<float> ls = L, rs = R;
        { StereoWidth s; s.prepare (kSR); s.setWidth (1.3f); s.setSat (1.0f); s.reset();
          for (int i = 0; i < N; i += kBlock) s.process (ls.data()+i, rs.data()+i, kBlock); }
        std::vector<float> l = L, r = R;
        StereoWidth fx; fx.prepare (kSR); fx.setWidth (1.3f); fx.reset();
        for (int i = 0; i < N; i += kBlock) { fx.setSat ((float) i / (float) N); fx.process (l.data()+i, r.data()+i, kBlock); }
        REQUIRE (tu::allFinite (l));
        REQUIRE (tu::maxDelta (l) < tu::maxDelta (ls) * 1.3f + 1.0e-3f);
    }
}

TEST_CASE ("StereoWidth: width>1 widens a DRY MONO source, mono-safe", "[6b][fx][width]")
{
    const int N = 8192;
    std::vector<float> mono (N);
    for (int i = 0; i < N; ++i)                       // a mono chord-ish source (harmonically rich)
        mono[(std::size_t) i] = 0.3f * (float) (std::sin (tu::kTwoPi * 180.0 * i / kSR)
                                              + std::sin (tu::kTwoPi * 270.0 * i / kSR)
                                              + std::sin (tu::kTwoPi * 410.0 * i / kSR));

    auto run = [&] (float w)
    {
        StereoWidth fx; fx.prepare (kSR); fx.setWidth (w); fx.reset();
        auto l = mono, r = mono;                     // L == R (dry mono)
        // Warm the smoother/allpass so we measure steady state, then measure.
        fx.process (l.data(), r.data(), N);
        return std::pair<std::vector<float>, std::vector<float>> { l, r };
    };

    SECTION ("dry mono: width=2 decorrelates L/R; width=1 leaves it mono")
    {
        auto [l1, r1] = run (1.0f);
        REQUIRE (correlation (l1, r1) > 0.999);       // still mono at unity
        auto [l2, r2] = run (2.0f);
        REQUIRE (correlation (l2, r2) < 0.9);         // now genuinely stereo
    }

    SECTION ("mono fold-down is unchanged (allpass, not Haas: no comb notches)")
    {
        auto [l2, r2] = run (2.0f);
        std::vector<float> sumWide (N), sumDry (N);
        for (int i = 0; i < N; ++i) { sumWide[(std::size_t) i] = l2[(std::size_t) i] + r2[(std::size_t) i];
                                      sumDry [(std::size_t) i] = 2.0f * mono[(std::size_t) i]; }
        REQUIRE (rmsDiff (sumWide, sumDry) < 1e-4);   // L+R == 2*mid: the widening cancels on mono
    }
}

TEST_CASE ("StereoWidth: width sweeps are click-free and allocation-free", "[6b][fx][width][rt]")
{
    const int N = 8192, block = 64;
    std::vector<float> L (N), R (N);
    for (int i = 0; i < N; ++i) L[(std::size_t) i] = R[(std::size_t) i] = 0.4f * (float) std::sin (tu::kTwoPi * 200.0 * i / kSR);

    StereoWidth fx; fx.prepare (kSR); fx.reset();
    float maxJump = 0.0f; float prev = 0.0f;
    {
        alloc_hook::AllocGuard g;
        for (int i = 0; i < N; i += block)
        {
            fx.setWidth (1.0f + (float) i / (float) N);   // sweep 1 -> 2 across the buffer
            fx.process (L.data() + i, R.data() + i, block);
        }
        REQUIRE (g.count() == 0);                          // RT-safe
    }
    for (int i = 0; i < N; ++i) { maxJump = std::max (maxJump, std::abs (L[(std::size_t) i] - prev)); prev = L[(std::size_t) i]; }
    REQUIRE (tu::allFinite (L));
    REQUIRE (tu::allFinite (R));
    REQUIRE (maxJump < 0.1f);                              // no discontinuity across the sweep
}

// ---------------------------------------------------------------------------
TEST_CASE ("Chorus: decorrelates a mono source and mix=0 is identity", "[6b][fx][chorus]")
{
    const int N = 8192;
    std::vector<float> L (N), R (N);
    fillMonoSine (L, R, 220.0);

    SECTION ("mix=0 passes dry unchanged")
    {
        Chorus fx; fx.prepare (kSR, kBlock);
        fx.setParams (1.0f, 1.0f, 0.0f); fx.reset();
        auto l = L, r = R;
        fx.process (l.data(), r.data(), N);
        REQUIRE (rmsDiff (l, L) < 1e-6);
    }

    SECTION ("wet output is stereo (L != R) and finite")
    {
        Chorus fx; fx.prepare (kSR, kBlock);
        fx.setParams (1.5f, 1.0f, 1.0f); fx.reset();
        auto l = L, r = R;
        for (int i = 0; i < N; i += kBlock) fx.process (l.data() + i, r.data() + i, kBlock);
        REQUIRE (tu::allFinite (l));
        REQUIRE (tu::allFinite (r));
        REQUIRE (rmsDiff (l, r) > 0.02);      // meaningfully decorrelated
    }
}

// ---------------------------------------------------------------------------
// Tier 4c — chorus VOICES: a second decorrelated tap per channel.
TEST_CASE ("Chorus VOICES: dual-tap widens, voices=1 bit-identical, click-safe toggle", "[6b][fx][chorus][voices]")
{
    const int N = 16384;
    std::vector<float> L (N), R (N); fillMonoSine (L, R, 330.0);

    auto lrCorr = [] (const std::vector<float>& a, const std::vector<float>& b, std::size_t start, std::size_t len)
    {
        double ll = 0, rr = 0, lr = 0;
        for (std::size_t i = start; i < start + len && i < a.size(); ++i) { ll += a[i]*a[i]; rr += b[i]*b[i]; lr += a[i]*b[i]; }
        return (ll > 1e-12 && rr > 1e-12) ? lr / std::sqrt (ll * rr) : 1.0;
    };
    auto renderInto = [&] (int voices, std::vector<float>& l, std::vector<float>& r)
    {
        l = L; r = R;
        Chorus fx; fx.prepare (kSR, kBlock); fx.setParams (0.8f, 0.7f, 1.0f, voices); fx.reset();
        for (int i = 0; i < N; i += kBlock) fx.process (l.data() + i, r.data() + i, kBlock);
    };

    SECTION ("voices=1 is bit-identical to the legacy 3-arg call")
    {
        std::vector<float> la = L, ra = R, lb = L, rb = R;
        Chorus a; a.prepare (kSR, kBlock); a.setParams (0.8f, 0.7f, 1.0f);    a.reset();
        Chorus b; b.prepare (kSR, kBlock); b.setParams (0.8f, 0.7f, 1.0f, 1); b.reset();
        for (int i = 0; i < N; i += kBlock) { a.process (la.data()+i, ra.data()+i, kBlock); b.process (lb.data()+i, rb.data()+i, kBlock); }
        REQUIRE (la == lb); REQUIRE (ra == rb);
    }

    SECTION ("voices=2 decorrelates L/R more than voices=1")
    {
        // Broadband mono input: a single chorus tap leaves L/R fairly correlated, so a second
        // decorrelated tap has room to widen (a pure sine is already near-zero correlation).
        std::uint32_t s = 22222u;
        for (int i = 0; i < N; ++i) { s = s * 1664525u + 1013904223u; const float v = (float) ((int) (s >> 9) & 0x3fff) / 8192.0f - 1.0f; L[(std::size_t) i] = 0.4f * v; R[(std::size_t) i] = L[(std::size_t) i]; }
        std::vector<float> l1, r1, l2, r2;
        renderInto (1, l1, r1);
        renderInto (2, l2, r2);
        const double c1 = lrCorr (l1, r1, (std::size_t) (N/2), (std::size_t) (N/2));
        const double c2 = lrCorr (l2, r2, (std::size_t) (N/2), (std::size_t) (N/2));
        const double changed = rmsDiff (tu::slice (l2, (std::size_t) (N/2), (std::size_t) (N/2)),
                                        tu::slice (l1, (std::size_t) (N/2), (std::size_t) (N/2)));
        INFO ("L/R correlation  voices1=" << c1 << "  voices2=" << c2 << "  rmsDiff(v1,v2)=" << changed);
        REQUIRE (c2 < c1 - 0.015);   // the second tap lowers L/R correlation (widens)
        REQUIRE (changed > 0.03);    // and it is a real, substantial second voice (thickening)
    }

    SECTION ("toggling voices mid-note is click-free")
    {
        std::vector<float> l = L, r = R;
        Chorus fx; fx.prepare (kSR, kBlock); fx.reset();
        for (int i = 0; i < N; i += kBlock) { fx.setParams (0.8f, 0.7f, 1.0f, (i < N/2) ? 1 : 2); fx.process (l.data()+i, r.data()+i, kBlock); }

        std::vector<float> ls = L, rs = R;   // single-tap baseline for a max-delta bound
        { Chorus s; s.prepare (kSR, kBlock); s.setParams (0.8f, 0.7f, 1.0f, 1); s.reset();
          for (int i = 0; i < N; i += kBlock) s.process (ls.data()+i, rs.data()+i, kBlock); }
        REQUIRE (tu::allFinite (l));
        REQUIRE (tu::maxDelta (l) < tu::maxDelta (ls) * 1.5f + 1.0e-4f);   // the toggle crossfades, no click
    }
}

// ---------------------------------------------------------------------------
TEST_CASE ("StereoDelay: echo timing, feedback repeats, ping-pong, stability", "[6b][fx][delay]")
{
    const int N = (int) (kSR * 0.6);
    const int delayMs = 100;
    const int delaySamp = (int) (kSR * delayMs / 1000.0);

    // Impulse on L only.
    std::vector<float> L (N, 0.0f), R (N, 0.0f);
    L[0] = 1.0f;

    SECTION ("wet echo appears one delay-time later")
    {
        StereoDelay fx; fx.prepare (kSR, kBlock);
        fx.setParams ((float) delayMs, 0.0f, 1.0f, 0.0f); fx.reset();   // no fb, no ping-pong, full wet
        auto l = L, r = R;
        for (int i = 0; i < N; i += kBlock) fx.process (l.data() + i, r.data() + i, std::min (kBlock, N - i));
        // energy near the delay position, ~nothing well before it
        const double early = tu::rms (tu::slice (l, 10, (std::size_t) delaySamp - 200));
        const double atEcho = tu::rms (tu::slice (l, (std::size_t) delaySamp - 50, 100));
        REQUIRE (atEcho > early * 10.0);
        REQUIRE (tu::allFinite (l));
    }

    SECTION ("ping-pong: an L impulse's first echo lands on R")
    {
        StereoDelay fx; fx.prepare (kSR, kBlock);
        fx.setParams ((float) delayMs, 0.5f, 1.0f, 1.0f); fx.reset();   // full spread
        auto l = L, r = R;
        for (int i = 0; i < N; i += kBlock) fx.process (l.data() + i, r.data() + i, std::min (kBlock, N - i));
        const double echoL = tu::rms (tu::slice (l, (std::size_t) delaySamp - 50, 100));
        const double echoR = tu::rms (tu::slice (r, (std::size_t) delaySamp - 50, 100));
        REQUIRE (echoR > echoL * 4.0);
        REQUIRE (tu::allFinite (l));
        REQUIRE (tu::allFinite (r));
    }

    SECTION ("high feedback stays bounded")
    {
        StereoDelay fx; fx.prepare (kSR, kBlock);
        fx.setParams (50.0f, 0.95f, 1.0f, 0.5f); fx.reset();
        std::vector<float> l (N, 0.0f), r (N, 0.0f); l[0] = 1.0f;
        for (int i = 0; i < N; i += kBlock) fx.process (l.data() + i, r.data() + i, std::min (kBlock, N - i));
        REQUIRE (tu::allFinite (l));
        REQUIRE (tu::peak (l) < 4.0f);        // decays, doesn't blow up
    }
}

// ---------------------------------------------------------------------------
TEST_CASE ("Reverb: decaying tail, mix=0 identity, stable and finite", "[6b][fx][reverb]")
{
    const int N = (int) (kSR * 2.0);

    SECTION ("mix=0 passes dry")
    {
        std::vector<float> L (N, 0.0f), R (N, 0.0f); L[0] = 1.0f;
        Reverb fx; fx.prepare (kSR);
        fx.setParams (0.8f, 0.5f, 1.0f, 0.0f); fx.reset();
        auto l = L;
        fx.process (l.data(), R.data(), N);
        REQUIRE (rmsDiff (l, L) < 1e-6);
    }

    SECTION ("impulse yields a tail that outlasts the dry and decays")
    {
        std::vector<float> L (N, 0.0f), R (N, 0.0f); L[0] = 1.0f; R[0] = 1.0f;
        Reverb fx; fx.prepare (kSR);
        fx.setParams (0.85f, 0.3f, 1.0f, 1.0f); fx.reset();
        for (int i = 0; i < N; i += kBlock) fx.process (L.data() + i, R.data() + i, std::min (kBlock, N - i));
        REQUIRE (tu::allFinite (L));
        const double early = tu::rms (tu::slice (L, (std::size_t) (kSR * 0.1), 4096));
        const double late  = tu::rms (tu::slice (L, (std::size_t) (kSR * 1.0), 4096));
        REQUIRE (early > 1e-4);               // there IS a tail well after the impulse
        REQUIRE (late  < early);              // and it decays
        REQUIRE (tu::peak (L) < 4.0f);        // bounded
    }
}

namespace
{
    // Peak-to-average magnitude of a windowed FFT of a decaying tail. A static Freeverb
    // rings on a few sharp, fixed comb resonances -> high peak/avg (reads metallic). MOTION
    // wanders those resonances, smearing them over the window -> lower peak/avg.
    // Unit-norm magnitude spectrum over a band — the SHAPE of the tail's spectrum, decay
    // envelope removed. Fixed comb resonances make this shape stationary; MOTION slowly
    // wanders the peaks so the shape at t1 and t2 differs.
    std::vector<double> normSpecBand (const std::vector<float>& mono, std::size_t start, int fftN,
                                      double sr, double fLo, double fHi)
    {
        std::vector<float> w = tu::slice (mono, start, (std::size_t) fftN);
        tu::blackmanHarris (w);
        std::vector<std::complex<double>> a ((std::size_t) fftN);
        for (int i = 0; i < fftN; ++i) a[(std::size_t) i] = { (double) w[(std::size_t) i], 0.0 };
        tu::fft (a);
        const int lo = std::max (1, (int) (fLo * fftN / sr));
        const int hi = std::min (fftN / 2, (int) (fHi * fftN / sr));
        std::vector<double> s; double e = 0.0;
        for (int i = lo; i < hi; ++i) { const double m = std::abs (a[(std::size_t) i]); s.push_back (m); e += m * m; }
        const double norm = std::sqrt (std::max (e, 1e-30));
        for (auto& v : s) v /= norm;
        return s;
    }

    // Crest (peak/mean) of the TIME-AVERAGED HF spectrum over the tail. Averaging many
    // short frames: static comb peaks always land in the same bins -> they survive the
    // average as sharp peaks (high crest); MOTION wanders them frame to frame -> they smear
    // into broad humps (low crest). This is the classic "modulation smears the spectrum"
    // and, being averaged over ~8 frames, does not depend on any single snapshot's LFO phase.
    double tailAvgCrest (const std::vector<float>& mono, double sr)
    {
        std::vector<double> acc; int frames = 0;
        for (double t = 1.0; t <= 7.0 + 1e-9; t += 0.10)
        {
            auto s = normSpecBand (mono, (std::size_t) (sr * t), 4096, sr, 2000.0, 12000.0);
            if (acc.empty()) acc.assign (s.size(), 0.0);
            for (std::size_t i = 0; i < s.size() && i < acc.size(); ++i) acc[i] += s[i];
            ++frames;
        }
        double sum = 0.0, peak = 0.0;
        for (double v : acc) { v /= frames; sum += v; peak = std::max (peak, v); }
        return acc.empty() ? 0.0 : peak / (sum / (double) acc.size());
    }

    // A held excitation (short noise burst, then silence) so the tail is long and broadband;
    // rendered long enough for the slow motion LFOs to complete cycles and wander the peaks.
    std::vector<float> reverbTail (float motion, double seconds = 8.0, float size = 0.9f, float damp = 0.0f)
    {
        const int N = (int) (kSR * seconds);
        std::vector<float> L ((std::size_t) N, 0.0f), R ((std::size_t) N, 0.0f);
        for (int i = 0; i < (int) (kSR * 0.02); ++i)          // 20 ms broadband burst
        { const float s = ((i * 2654435761u >> 8) & 1) ? 0.5f : -0.5f; L[(std::size_t) i] = s; R[(std::size_t) i] = s; }
        Reverb fx; fx.prepare (kSR);
        fx.setParams (size, damp, 1.0f, 1.0f, motion); fx.reset();
        for (int i = 0; i < N; i += kBlock) fx.process (L.data() + i, R.data() + i, std::min (kBlock, N - i));
        return L;
    }
}

// ---------------------------------------------------------------------------
// Tier 4a — reverb MOTION: slow, small, per-line modulation of a subset of combs.
TEST_CASE ("Reverb MOTION: smears the metallic tail, click-safe, default is bit-identical", "[6b][fx][reverb][motion]")
{
    SECTION ("motion=0 is bit-identical to the classic (un-modulated) path")
    {
        const int N = (int) (kSR * 0.5);
        std::vector<float> L1 ((std::size_t) N, 0.0f), R1 ((std::size_t) N, 0.0f); L1[0] = R1[0] = 1.0f;
        std::vector<float> L2 = L1, R2 = R1;
        Reverb a; a.prepare (kSR); a.setParams (0.85f, 0.3f, 1.0f, 1.0f);       a.reset();   // classic 4-arg
        Reverb b; b.prepare (kSR); b.setParams (0.85f, 0.3f, 1.0f, 1.0f, 0.0f); b.reset();   // explicit motion 0
        for (int i = 0; i < N; i += kBlock)
        {
            const int n = std::min (kBlock, N - i);
            a.process (L1.data() + i, R1.data() + i, n);
            b.process (L2.data() + i, R2.data() + i, n);
        }
        REQUIRE (L1 == L2);   // sample-for-sample identical -> goldens hold
        REQUIRE (R1 == R2);
    }

    SECTION ("motion smears the fixed comb peaks in the time-averaged spectrum")
    {
        // Average the HF spectrum over ~8 frames of the tail: static peaks sit in the same
        // bins every frame and survive as sharp peaks (high crest); motion wanders them so
        // they smear into broad humps (lower crest).
        const auto tStatic = reverbTail (0.0f);
        const auto tMotion = reverbTail (1.0f);
        const double crestStatic = tailAvgCrest (tStatic, kSR);
        const double crestMotion = tailAvgCrest (tMotion, kSR);
        INFO ("rmsDiff(static,motion)=" << rmsDiff (tStatic, tMotion)
              << "  time-avg HF crest  static=" << crestStatic << "  motion=" << crestMotion);
        REQUIRE (crestMotion < crestStatic * 0.85);   // motion measurably smears the peaks (the cure)
    }

    SECTION ("sweeping MOTION live adds no click")
    {
        const int N = (int) (kSR * 1.0);
        std::vector<float> L ((std::size_t) N), R ((std::size_t) N); fillMonoSine (L, R, 220.0);

        auto Ls = L, Rs = R;   // static reference for a max-delta baseline
        { Reverb s; s.prepare (kSR); s.setParams (0.85f, 0.3f, 1.0f, 0.5f, 0.0f); s.reset();
          for (int i = 0; i < N; i += kBlock) s.process (Ls.data() + i, Rs.data() + i, std::min (kBlock, N - i)); }

        auto Lm = L, Rm = R;   // sweep motion 0 -> 1 across the render
        Reverb fx; fx.prepare (kSR); fx.reset();
        for (int i = 0; i < N; i += kBlock)
        {
            fx.setParams (0.85f, 0.3f, 1.0f, 0.5f, (float) i / (float) N);
            fx.process (Lm.data() + i, Rm.data() + i, std::min (kBlock, N - i));
        }
        REQUIRE (tu::allFinite (Lm));
        REQUIRE (tu::maxDelta (Lm) < tu::maxDelta (Ls) * 1.5f + 1.0e-4f);   // a click would spike far past this
    }

    SECTION ("a fully modulated decay stays finite and settles (denormal-safe)")
    {
        const int N = (int) (kSR * 3.0);
        std::vector<float> L ((std::size_t) N, 0.0f), R ((std::size_t) N, 0.0f); L[0] = R[0] = 1.0f;
        Reverb fx; fx.prepare (kSR); fx.setParams (0.7f, 0.5f, 1.0f, 1.0f, 1.0f); fx.reset();
        for (int i = 0; i < N; i += kBlock) fx.process (L.data() + i, R.data() + i, std::min (kBlock, N - i));
        REQUIRE (tu::allFinite (L));
        REQUIRE (tu::rms (tu::slice (L, (std::size_t) (kSR * 2.5), 8192)) < 1.0e-3);   // decays to silence, no stuck energy
    }
}

// ---------------------------------------------------------------------------
TEST_CASE ("FXChain: bypass identity, order matters, and reorder is click-free", "[6b][fx][chain]")
{
    const int N = 8192;
    std::vector<float> L (N), R (N);
    fillMonoSine (L, R, 200.0);

    SECTION ("all-disabled chain is a pass-through")
    {
        FXChain chain; chain.prepare (kSR, kBlock);
        FXParams p;                              // all enabled=false by default
        chain.setParams (p);
        auto l = L, r = R;
        for (int i = 0; i < N; i += kBlock) chain.process (l.data() + i, r.data() + i, kBlock);
        REQUIRE (rmsDiff (l, L) < 1e-6);
        REQUIRE_FALSE (chain.isCrossfading());
    }

    SECTION ("order matters for non-commuting effects (chorus vs delay)")
    {
        auto render = [&] (int a, int b)
        {
            FXChain chain; chain.prepare (kSR, kBlock);
            FXParams p;
            p.enabled[FXChain::Chorus_] = true;
            p.enabled[FXChain::Delay_]  = true;
            p.chorusMix = 0.6f; p.delayMix = 0.5f; p.delayFeedback = 0.4f;
            p.order[0] = a; p.order[1] = b; p.order[2] = 2; p.order[3] = 3;
            chain.setParams (p);
            auto l = L, r = R;
            for (int i = 0; i < N; i += kBlock) chain.process (l.data() + i, r.data() + i, kBlock);
            return l;
        };
        auto cd = render (FXChain::Chorus_, FXChain::Delay_);
        auto dc = render (FXChain::Delay_, FXChain::Chorus_);
        REQUIRE (rmsDiff (cd, dc) > 1e-3);       // the order audibly changes the result
    }

    SECTION ("reordering mid-stream adds no click vs. the steady chain")
    {
        auto runWithReorder = [&] (bool doReorder)
        {
            FXChain chain; chain.prepare (kSR, kBlock);
            FXParams p;
            p.enabled[FXChain::Chorus_] = true;
            p.enabled[FXChain::Width_]  = true;
            p.chorusMix = 0.5f; p.width = 1.6f;
            p.order[0] = FXChain::Chorus_; p.order[1] = FXChain::Width_; p.order[2] = 2; p.order[3] = 3;
            chain.setParams (p);
            std::vector<float> l (N), r (N);
            for (int i = 0; i < N; ++i) { l[(std::size_t) i] = L[(std::size_t) i]; r[(std::size_t) i] = R[(std::size_t) i]; }
            for (int i = 0; i < N; i += kBlock)
            {
                if (doReorder && i == N / 2)     // swap chorus/width halfway
                {
                    std::swap (p.order[0], p.order[1]);
                    chain.setParams (p);
                }
                chain.process (l.data() + i, r.data() + i, kBlock);
            }
            return l;
        };

        const float steadyDelta = tu::maxDelta (runWithReorder (false));
        const float reorderDelta = tu::maxDelta (runWithReorder (true));
        // The crossfade must keep the reorder's worst sample step close to the
        // steady chain's — a hard switch would spike this well above.
        REQUIRE (reorderDelta < steadyDelta * 1.5f + 0.02f);
    }

    // K1: the per-part EQ is a FIXED final stage. Its slot in order[] must not change
    // the result — put EQ first vs. last, everything else equal; the output is identical.
    SECTION ("EQ position in order[] is inert (always applied last)")
    {
        auto render = [&] (int eqSlot)
        {
            FXChain chain; chain.prepare (kSR, kBlock);
            FXParams p;
            p.enabled[FXChain::EQ_]     = true;
            p.enabled[FXChain::Chorus_] = true;
            p.chorusMix = 0.6f;
            p.eqBand2 = { 1000.0f, 12.0f, 2.0f, true };   // an audible boost at the tone
            // Build an order with EQ pinned at eqSlot and the other four filling around it.
            int fill[4] { FXChain::Chorus_, FXChain::Delay_, FXChain::Reverb_, FXChain::Width_ };
            int fi = 0;
            for (int s = 0; s < 5; ++s) p.order[s] = (s == eqSlot) ? FXChain::EQ_ : fill[fi++];
            chain.setParams (p);
            auto l = L, r = R;
            for (int i = 0; i < N; i += kBlock) chain.process (l.data() + i, r.data() + i, kBlock);
            return l;
        };
        auto eqFirst = render (0);
        auto eqLast  = render (4);
        REQUIRE (rmsDiff (eqFirst, eqLast) < 1e-6);   // position-independent -> migration is a no-op
    }

    // A boosted EQ band must audibly lift the tone at its centre when enabled.
    SECTION ("EQ boost lifts the tone; disabled EQ is transparent")
    {
        auto render = [&] (bool eqOn)
        {
            FXChain chain; chain.prepare (kSR, kBlock);
            FXParams p;
            p.enabled[FXChain::EQ_] = eqOn;
            p.eqBand1 = { 200.0f, 12.0f, 1.5f, true };   // boost at the 200 Hz tone
            chain.setParams (p);
            auto l = L, r = R;
            for (int i = 0; i < N; i += kBlock) chain.process (l.data() + i, r.data() + i, kBlock);
            return l;
        };
        auto boosted = render (true);
        auto flat    = render (false);
        REQUIRE (rmsDiff (flat, L) < 1e-6);            // disabled -> bit-transparent
        REQUIRE (tu::rms (boosted) > tu::rms (flat) * 1.3f);
    }
}

// ---------------------------------------------------------------------------
TEST_CASE ("FXChain: process (incl. crossfade) performs no heap allocation", "[6b][fx][rt]")
{
    FXChain chain; chain.prepare (kSR, kBlock);
    FXParams p;
    p.enabled[FXChain::Chorus_] = true;
    p.enabled[FXChain::Delay_]  = true;
    p.enabled[FXChain::Reverb_] = true;
    p.enabled[FXChain::Width_]  = true;
    p.chorusMix = 0.4f; p.delayMix = 0.3f; p.reverbMix = 0.3f;
    chain.setParams (p);

    std::vector<float> l (kBlock, 0.2f), r (kBlock, 0.2f);

    // Warm up (first blocks may still be settling), then arm the counter.
    for (int i = 0; i < 8; ++i) chain.process (l.data(), r.data(), kBlock);

    {
        alloc_hook::AllocGuard g;
        for (int b = 0; b < 200; ++b)
        {
            if (b == 20)                         // force a reorder -> crossfade path
            {
                std::swap (p.order[0], p.order[2]);
                chain.setParams (p);
            }
            chain.process (l.data(), r.data(), kBlock);
        }
        REQUIRE (g.count() == 0);
    }
}
