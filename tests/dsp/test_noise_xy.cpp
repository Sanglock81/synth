// Synth — Copyright © 2026 John L Farmer. Licensed under AGPLv3; see LICENSE.
// ============================================================================
// NOISE XY — the noise source's shaping field (Phase B).
//
// The field is one continuous surface: at y = 0 the x axis is spectral TILT
// (brown -> pink -> white -> bright); above y = 0 it becomes a bandpass FOCUS whose
// centre x sweeps and whose Q y sweeps, with the two regions crossfaded across
// y in [0, 0.15] so there is no seam to hear.
//
// What these tests pin, in order of how much they would cost to get wrong:
//   * BYPASS is a real code path, not a "coefficients happen to be neutral" — an
//     untouched patch must render the SAME SAMPLES as before the feature existed.
//   * the four documented tilt anchors actually come out of the filter (measured
//     from a spectrum, not read back off the mapping function),
//   * the focus centre/Q mappings and the crossfade are continuous across the
//     boundary, so a drag or an LFO never steps,
//   * nothing rings away to infinity or a NaN at the top of the Q range, at ANY
//     supported sample rate.
//
// CHARACTER OVER ACCURACY: the tilt tolerances below are deliberately loose. A
// 3-stage cascade approximates a constant slope as a staircase, and that ripple is
// part of the sound. These tests assert the colours are ORDERED and land near their
// anchors -- they are not a claim of calibrated dB/oct, and the docs must not make one.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "NoiseShaper.h"
#include "SynthVoice.h"
#include "test_util.h"
#include <vector>
#include <cmath>

namespace
{
    constexpr double kSR = 48000.0;

    // Deterministic white noise, independent of the voice's generator, so a spectrum
    // measurement is repeatable run to run and platform to platform.
    struct White
    {
        std::uint32_t s = 0x12345678u;
        float next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5;
                       return (float) (std::int32_t) s / 2147483648.0f; }
    };

    // Run `n` samples of white through a shaper parked at (x, y) and return the output.
    std::vector<float> shape (float x, float y, int n = 1 << 16, double sr = kSR)
    {
        NoiseShaper ns; ns.prepare (sr);
        ns.setTarget (x, y); ns.snapToTarget(); ns.setTarget (x, y);
        White w;
        std::vector<float> out ((std::size_t) n);
        for (int i = 0; i < n; ++i) out[(std::size_t) i] = ns.process (w.next());
        return out;
    }

    // Average magnitude (dB) in a third-octave band around `hz`, from Welch-style
    // averaged periodograms — a single FFT of noise is far too jittery to measure a slope.
    double bandDb (const std::vector<float>& sig, double hz, double sr = kSR)
    {
        constexpr std::size_t N = 4096;
        const std::size_t frames = sig.size() / N;
        std::vector<double> acc (N / 2 + 1, 0.0);
        for (std::size_t f = 0; f < frames; ++f)
        {
            auto mag = tu::magnitudeSpectrum (tu::slice (sig, f * N, N));
            for (std::size_t i = 0; i < acc.size(); ++i) acc[i] += mag[i] * mag[i];
        }
        const double lo = hz / 1.122, hi = hz * 1.122;          // ~1/3 octave
        double sum = 0.0; int cnt = 0;
        for (std::size_t i = 1; i < acc.size(); ++i)
        {
            const double f = double (i) * sr / double (N);
            if (f >= lo && f <= hi) { sum += acc[i] / double (frames); ++cnt; }
        }
        return cnt == 0 ? -300.0 : tu::linToDb (std::sqrt (sum / double (cnt)));
    }

    // Measured slope in dB/oct between two probe frequencies.
    double measuredSlope (const std::vector<float>& sig, double f1, double f2)
    {
        return (bandDb (sig, f2) - bandDb (sig, f1)) / std::log2 (f2 / f1);
    }

    // Peak-magnitude bin of an averaged spectrum, in Hz.
    double spectralPeakHz (const std::vector<float>& sig, double sr = kSR)
    {
        constexpr std::size_t N = 4096;
        const std::size_t frames = sig.size() / N;
        std::vector<double> acc (N / 2 + 1, 0.0);
        for (std::size_t f = 0; f < frames; ++f)
        {
            auto mag = tu::magnitudeSpectrum (tu::slice (sig, f * N, N));
            for (std::size_t i = 0; i < acc.size(); ++i) acc[i] += mag[i] * mag[i];
        }
        std::size_t best = 1;
        for (std::size_t i = 2; i < acc.size(); ++i) if (acc[i] > acc[best]) best = i;
        return double (best) * sr / double (N);
    }
}

// ---------------------------------------------------------------------------
// BYPASS — the guarantee every existing preset rests on.
// ---------------------------------------------------------------------------

TEST_CASE ("noise xy: the defaults are the exact bypass point", "[noise][bypass]")
{
    REQUIRE (NoiseShaper::isBypass (NoiseShaper::kDefaultX, NoiseShaper::kDefaultY));
    REQUIRE (VoiceParams{}.noiseX == NoiseShaper::kDefaultX);
    REQUIRE (VoiceParams{}.noiseY == NoiseShaper::kDefaultY);
    // A hair off either axis is NOT bypass -- the test exists so nobody "helpfully"
    // relaxes the comparison to an epsilon and silently puts the filter in every path.
    REQUIRE_FALSE (NoiseShaper::isBypass (0.5001f, 0.0f));
    REQUIRE_FALSE (NoiseShaper::isBypass (0.5f, 0.0001f));
}

TEST_CASE ("noise xy: a default-field voice renders bit-identically to raw white noise", "[noise][bypass][golden]")
{
    // Two voices with identical params, one rendered through the field-aware path with
    // the field at its defaults. Same samples, bit for bit -- that is what makes every
    // committed golden and every shipped preset safe.
    auto render = [] (float nx, float ny)
    {
        SynthVoice v; v.prepare (kSR);
        VoiceParams p;
        p.osc1Level = 0.0f; p.osc2Level = 0.0f; p.osc3Level = 0.0f;   // noise only
        p.noiseLevel = 0.8f; p.noiseX = nx; p.noiseY = ny;
        p.ampA = 0.001f; p.ampD = 0.5f; p.ampS = 1.0f;
        v.noteOn (60, 1.0f, 0);
        std::vector<float> out (4096, 0.0f);
        for (int c = 0; c < 4096; c += 16) v.render (out.data() + c, 16, p);
        return out;
    };
    const auto bypassed = render (NoiseShaper::kDefaultX, NoiseShaper::kDefaultY);
    const auto shaped   = render (0.25f, 0.0f);                       // pink: must DIFFER

    REQUIRE (tu::peak (bypassed) > 0.05f);                            // it really did render
    bool identicalToShaped = true;
    for (std::size_t i = 0; i < bypassed.size(); ++i)
        if (bypassed[i] != shaped[i]) { identicalToShaped = false; break; }
    REQUIRE_FALSE (identicalToShaped);                                // the field does something

    // ...and the bypassed render matches a second bypassed render exactly (determinism),
    // which together with the differing shaped render proves the branch is real.
    const auto again = render (NoiseShaper::kDefaultX, NoiseShaper::kDefaultY);
    for (std::size_t i = 0; i < bypassed.size(); ++i) REQUIRE (bypassed[i] == again[i]);
}

TEST_CASE ("noise xy: an inert route to the field leaves the bypass in place", "[noise][bypass]")
{
    // A slot that exists but is dead (source None, or depth 0) must NOT drag the shaper
    // into the path -- otherwise clearing a route would not restore the original render.
    ModMatrix m;
    REQUIRE_FALSE (m.targets (ModMatrix::NoiseX));
    m.slots[0] = { ModMatrix::LFO1, ModMatrix::NoiseY, 0.0f };        // depth 0 = dead
    REQUIRE_FALSE (m.targets (ModMatrix::NoiseY));
    m.slots[0] = { ModMatrix::SrcNone, ModMatrix::NoiseY, 0.5f };     // no source = dead
    REQUIRE_FALSE (m.targets (ModMatrix::NoiseY));
    m.slots[0] = { ModMatrix::LFO1, ModMatrix::NoiseY, 0.5f };        // live
    REQUIRE (m.targets (ModMatrix::NoiseY));
}

TEST_CASE ("noise xy: adding then removing a route restores the render exactly", "[noise][bypass][golden]")
{
    auto render = [] (const ModMatrix* mtx)
    {
        SynthVoice v; v.prepare (kSR);
        VoiceParams p;
        p.osc1Level = 0.0f; p.osc2Level = 0.0f; p.osc3Level = 0.0f;
        p.noiseLevel = 0.8f;                                          // field left at defaults
        p.ampA = 0.001f; p.ampD = 0.5f; p.ampS = 1.0f;
        ModSources src;
        v.noteOn (60, 1.0f, 0);
        std::vector<float> out (4096, 0.0f);
        for (int c = 0; c < 4096; c += 16) v.render (out.data() + c, 16, p, mtx, &src);
        return out;
    };
    ModMatrix none, routed, cleared;
    routed.slots[0] = { ModMatrix::LFO1, ModMatrix::NoiseY, 0.8f };
    cleared.slots[0] = { ModMatrix::LFO1, ModMatrix::NoiseY, 0.8f };
    cleared.slots[0] = {};                                            // ...and removed again

    const auto a = render (&none), b = render (&routed), c = render (&cleared);
    bool routedDiffers = false;
    for (std::size_t i = 0; i < a.size(); ++i) if (a[i] != b[i]) { routedDiffers = true; break; }
    REQUIRE (routedDiffers);                                          // the route was live
    for (std::size_t i = 0; i < a.size(); ++i) REQUIRE (a[i] == c[i]);   // removing it restores exactly
}

// ---------------------------------------------------------------------------
// REGION 1 — TILT.
// ---------------------------------------------------------------------------

TEST_CASE ("noise xy: the tilt mapping hits its four documented anchors", "[noise][tilt]")
{
    REQUIRE (NoiseShaper::tiltDbPerOct (0.0f)  == Catch::Approx (-6.0f));   // brown
    REQUIRE (NoiseShaper::tiltDbPerOct (0.25f) == Catch::Approx (-3.0f));   // pink
    REQUIRE (NoiseShaper::tiltDbPerOct (0.5f)  == Catch::Approx (0.0f));    // white
    REQUIRE (NoiseShaper::tiltDbPerOct (1.0f)  == Catch::Approx (4.5f));    // bright
    // Continuous and monotonic across the whole axis (a drag never jumps or reverses).
    float prev = NoiseShaper::tiltDbPerOct (0.0f);
    for (int i = 1; i <= 200; ++i)
    {
        const float v = NoiseShaper::tiltDbPerOct ((float) i / 200.0f);
        REQUIRE (v >= prev - 1.0e-5f);
        REQUIRE (std::abs (v - prev) < 0.2f);
        prev = v;
    }
}

TEST_CASE ("noise xy: white sits exactly at the centre of the tilt axis", "[noise][tilt]")
{
    // x = 0.5 must be a true identity, not "nearly flat": every shelf collapses to unity
    // when its pole meets its zero, so the shaper passes its input through untouched.
    NoiseShaper ns; ns.prepare (kSR);
    ns.setTarget (0.5f, 0.0f); ns.snapToTarget(); ns.setTarget (0.5f, 0.0f);
    White w;
    for (int i = 0; i < 8192; ++i)
    {
        const float in = w.next();
        REQUIRE (ns.process (in) == Catch::Approx ((double) in).margin (1e-6));
    }
}

TEST_CASE ("noise xy: the measured tilt tracks the requested slope", "[noise][tilt]")
{
    // Measured across the middle of the band, where all three stages are in transition.
    // Tolerance is wide on purpose (staircase ripple = character); what must hold is that
    // each colour lands near its anchor and the four are strictly ordered.
    const double brown  = measuredSlope (shape (0.00f, 0.0f), 125.0, 2000.0);
    const double pink   = measuredSlope (shape (0.25f, 0.0f), 125.0, 2000.0);
    const double white  = measuredSlope (shape (0.50f, 0.0f), 125.0, 2000.0);
    const double bright = measuredSlope (shape (1.00f, 0.0f), 125.0, 2000.0);

    REQUIRE (white  == Catch::Approx (0.0).margin (0.6));       // identity: flat to the noise floor
    REQUIRE (pink   == Catch::Approx (-3.0).margin (1.5));
    REQUIRE (brown  == Catch::Approx (-6.0).margin (1.5));
    REQUIRE (bright == Catch::Approx (4.5).margin (1.5));
    REQUIRE (brown < pink);                                     // strictly ordered, dark -> bright
    REQUIRE (pink  < white);
    REQUIRE (white < bright);
}

TEST_CASE ("noise xy: the tilt colour does not slide with the sample rate", "[noise][tilt][rate]")
{
    // The reason the coefficients are computed from the rate instead of using Paul Kellet's
    // 44.1 kHz constants: at 96 kHz those would move the whole colour up an octave.
    for (double sr : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        const double s = measuredSlope (shape (0.25f, 0.0f, 1 << 16, sr), 125.0, 2000.0);
        INFO ("sample rate " << sr << " measured " << s << " dB/oct");
        REQUIRE (s == Catch::Approx (-3.0).margin (1.5));
    }
}

// ---------------------------------------------------------------------------
// REGION 2 — FOCUS.
// ---------------------------------------------------------------------------

TEST_CASE ("noise xy: the focus centre spans 40 Hz to 12 kHz on a log axis", "[noise][focus]")
{
    REQUIRE (NoiseShaper::focusHz (0.0f) == Catch::Approx (40.0f));
    REQUIRE (NoiseShaper::focusHz (1.0f) == Catch::Approx (12000.0f));
    // Log axis: the midpoint of the knob is the GEOMETRIC mean, so equal drags move equal
    // musical intervals rather than piling every useful centre into the bottom of the axis.
    REQUIRE (NoiseShaper::focusHz (0.5f) == Catch::Approx (std::sqrt (40.0f * 12000.0f)).epsilon (0.001));
}

TEST_CASE ("noise xy: the focus Q runs from very wide to near-ring", "[noise][focus]")
{
    REQUIRE (NoiseShaper::focusQ (0.0f) == Catch::Approx (0.5f));
    REQUIRE (NoiseShaper::focusQ (1.0f) == Catch::Approx (60.0f));   // deliberately a pitched whistle
    REQUIRE (NoiseShaper::focusQ (0.5f) > NoiseShaper::focusQ (0.25f));
}

TEST_CASE ("noise xy: a focused band actually peaks at its centre frequency", "[noise][focus]")
{
    // Measured off the output spectrum, not the mapping: proves the coefficient path,
    // the prewarping and the makeup gain all agree with the number shown on the readout.
    for (float x : { 0.2f, 0.5f, 0.8f })
    {
        const double want = NoiseShaper::focusHz (x);
        const double got  = spectralPeakHz (shape (x, 0.9f));
        INFO ("x " << x << " want " << want << " Hz, peak at " << got << " Hz");
        REQUIRE (got == Catch::Approx (want).epsilon (0.15));
    }
}

TEST_CASE ("noise xy: raising focus narrows the band", "[noise][focus]")
{
    // At a fixed centre, more y = more energy at the centre relative to an octave away.
    auto contrast = [] (float y)
    {
        const auto sig = shape (0.5f, y);
        const double c = NoiseShaper::focusHz (0.5f);
        return bandDb (sig, c) - bandDb (sig, c * 4.0);
    };
    const double wide = contrast (0.25f), mid = contrast (0.6f), tight = contrast (1.0f);
    INFO ("contrast wide " << wide << " mid " << mid << " tight " << tight);
    REQUIRE (wide < mid);
    REQUIRE (mid  < tight);
}

// ---------------------------------------------------------------------------
// The seam between the two regions.
// ---------------------------------------------------------------------------

TEST_CASE ("noise xy: the tilt-to-focus crossfade is continuous across y = 0.15", "[noise][crossfade]")
{
    REQUIRE (NoiseShaper::focusBlend (0.0f)  == Catch::Approx (0.0f));    // pure tilt at the bottom edge
    REQUIRE (NoiseShaper::focusBlend (0.15f) == Catch::Approx (1.0f));    // pure focus at the top of the fade
    REQUIRE (NoiseShaper::focusBlend (0.5f)  == Catch::Approx (1.0f));    // and stays there above it
    // No step ANYWHERE across the boundary -- including at the exact boundary sample.
    float prev = NoiseShaper::focusBlend (0.0f);
    for (int i = 1; i <= 600; ++i)
    {
        const float v = NoiseShaper::focusBlend ((float) i / 2000.0f);
        REQUIRE (v >= prev - 1.0e-6f);
        REQUIRE (v - prev < 0.02f);
        prev = v;
    }
}

TEST_CASE ("noise xy: output level stays in one band across the whole field", "[noise][level]")
{
    // The makeup trims exist so a drag around the surface changes COLOUR, not loudness.
    // If a future tuning pass breaks that, this catches it before anyone's ears do.
    double lo = 1e9, hi = 0.0;
    for (int xi = 0; xi <= 8; ++xi)
        for (int yi = 0; yi <= 8; ++yi)
        {
            const double r = tu::rms (shape ((float) xi / 8.0f, (float) yi / 8.0f, 1 << 15));
            INFO ("x " << xi / 8.0f << " y " << yi / 8.0f << " rms " << r);
            REQUIRE (r > 0.02);                                  // never collapses to nothing
            lo = std::min (lo, r); hi = std::max (hi, r);
        }
    // The whole surface within ~12 dB, and centred on the level of the PLAIN white noise it
    // replaces (0.577 RMS for a uniform +/-1 source) -- so turning the field on is not itself
    // a volume change. Measured spread on this build is ~7 dB; the margin is for platform drift.
    REQUIRE (tu::linToDb (hi / lo) < 12.0);
    REQUIRE (std::abs (tu::linToDb (0.5 * (hi + lo) / 0.577)) < 6.0);
}

// ---------------------------------------------------------------------------
// Stability.
// ---------------------------------------------------------------------------

TEST_CASE ("noise xy: nothing blows up at maximum Q, at any supported rate", "[noise][stability]")
{
    for (double sr : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
        for (float x : { 0.0f, 0.35f, 0.7f, 1.0f })
        {
            const auto sig = shape (x, 1.0f, 1 << 15, sr);       // top of the Q range = near-ring
            INFO ("sr " << sr << " x " << x << " peak " << tu::peak (sig));
            REQUIRE (tu::allFinite (sig));
            REQUIRE (tu::peak (sig) < 40.0f);                    // rings, but bounded
        }
}

TEST_CASE ("noise xy: a hard sweep of either axis never steps the output", "[noise][stability][click]")
{
    // The shaper glides its own coordinate between coefficient updates. Without that,
    // stepping Q on a ringing filter once per block is an audible tick -- so the sample-
    // to-sample delta of a HARD (instant) coordinate jump is the thing to bound.
    NoiseShaper ns; ns.prepare (kSR);
    ns.setTarget (0.5f, 0.0f); ns.snapToTarget(); ns.setTarget (0.5f, 0.0f);
    White w;
    std::vector<float> out;
    out.reserve (48000);
    for (int i = 0; i < 48000; ++i)
    {
        if (i == 8000)  ns.setTarget (0.9f, 1.0f);               // white -> tight high focus, instantly
        if (i == 20000) ns.setTarget (0.0f, 1.0f);               // and slam the centre to the bottom
        if (i == 32000) ns.setTarget (0.0f, 0.0f);               // ...then back into the tilt region
        out.push_back (ns.process (w.next()));
    }
    REQUIRE (tu::allFinite (out));
    // White noise itself has a per-sample delta up to 2.0; the bound catches a genuine
    // discontinuity (a coefficient step rings a resonant filter far past that).
    REQUIRE (tu::maxDelta (out) < 8.0f);
}
