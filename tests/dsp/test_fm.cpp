// Synth — Copyright © 2026 John L Farmer. Licensed under AGPLv3; see LICENSE.
// ============================================================================
// #132 Osc phase-modulation (FM) chain — DSP proof.
//
// The synth's "FM" is DX-style PHASE modulation: a modulator oscillator's output
// offsets a carrier's read phase while the carrier's accumulator advances
// unmodulated (PolyBlepOscillator::nextSample(phaseOffset)). The voice wires a
// osc3 -> osc2 -> osc1 chain; depth (0..1) maps to +/- depth cycles of phase
// (modulation index beta = 2*pi*depth at a full-scale sine modulator).
//
// This file proves, at the oscillator primitive and through the engine:
//   * a sine carrier + sine modulator makes symmetric sidebands at fc +/- n*fmod;
//   * depth 0 is bit-identical to the un-modulated oscillator (goldens frozen);
//   * more depth = a broader spectrum (more sideband energy);
//   * an inharmonic ratio makes inharmonic (bell-like) partials;
//   * the sidebands KEYTRACK — a fixed SEMI/OCTAVE ratio scales with the note;
//   * a sine carrier under PM never exceeds +/-1 (safety), and a WT/tri carrier
//     at a high note + max depth stays finite + bounded;
//   * through the engine, the chain actually changes timbre with an INAUDIBLE
//     modulator (level 0), and the SIN/TRI/WT carrier restriction holds (a saw
//     carrier is inert — bit-identical to FM off).
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PolyBlepOscillator.h"
#include "SynthEngine.h"
#include "SynthVoice.h"
#include "test_util.h"
#include <vector>
#include <cmath>

namespace
{
    constexpr double kSR = 48000.0;

    // Raw phase-modulation of one carrier by one modulator, mirroring the voice EXACTLY:
    // phaseOffset (cycles) = depth * kFmDepthToCycles * modOut, with kFmDepthToCycles = 1.
    std::vector<float> renderPM (PolyBlepOscillator::Wave carrierWave, double fc, double fmod,
                                 float depth, int n,
                                 PolyBlepOscillator::Quality q = PolyBlepOscillator::Quality::Efficient)
    {
        PolyBlepOscillator carrier, mod;
        carrier.setQuality (q); mod.setQuality (q);
        carrier.prepare (kSR);  mod.prepare (kSR);
        carrier.setWave (carrierWave); mod.setWave (PolyBlepOscillator::Wave::Sine);
        carrier.setFrequency (fc);     mod.setFrequency (fmod);
        const float k = depth * SynthVoice::kFmDepthToCycles;
        std::vector<float> out ((std::size_t) n);
        for (int i = 0; i < n; ++i)
        {
            const double m = mod.nextSample();
            out[(std::size_t) i] = carrier.nextSample ((double) (k * (float) m));
        }
        return out;
    }

    // Magnitude spectrum in dB relative to the peak bin, Blackman-Harris windowed
    // (low leakage so sidebands read cleanly). Index by FFT bin.
    std::vector<double> spectrumDb (const std::vector<float>& sig)
    {
        auto mag = tu::magnitudeSpectrumWin (sig, tu::blackmanHarris);
        double mx = 0.0; for (double m : mag) mx = std::max (mx, m);
        std::vector<double> db (mag.size());
        for (std::size_t i = 0; i < mag.size(); ++i) db[i] = tu::linToDb (mag[i] / std::max (mx, 1e-30));
        return db;
    }

    int binFor (double hz, int n) { return (int) std::lround (hz / (kSR / (double) n)); }

    // Peak dB within +/- a few bins of a target frequency (BH main lobe tolerance).
    double peakNearDb (const std::vector<double>& db, double hz, int n, int win = 3)
    {
        const int b = binFor (hz, n);
        double p = -1e30;
        for (int j = std::max (0, b - win); j <= std::min ((int) db.size() - 1, b + win); ++j)
            p = std::max (p, db[(std::size_t) j]);
        return p;
    }

    // Engine render of one sustained note with a wide-open filter (so the raw osc
    // spectrum, not the filter, is what we measure). Returns a 2^13 sustain slice.
    std::vector<float> renderVoice (VoiceParams p, int note = 57, float vel = 1.0f)
    {
        SynthEngine e;
        e.prepare (kSR);
        e.noteOn (note, vel);
        std::vector<float> out; out.reserve (20000);
        const int block = 512;
        std::vector<float> buf ((std::size_t) block);
        for (int done = 0; done < 20000; done += block)
        {
            std::fill (buf.begin(), buf.end(), 0.0f);
            e.render (buf.data(), block, p, 2.0f, 0, 0.0f, 0);
            out.insert (out.end(), buf.begin(), buf.end());
        }
        return tu::slice (out, 8192, 8192);
    }

    VoiceParams openFilter (VoiceParams p)
    {
        p.filterType = 0; p.cutoffHz = 20000.0f; p.resonance = 0.0f;
        p.filterEnvAmt = 0.0f; p.keytrack = 0.0f; p.velToCutoff = 0.0f;
        p.ampA = 0.002f; p.ampD = 0.02f; p.ampS = 1.0f; p.ampR = 0.1f;   // flat sustain
        return p;
    }

    double centroidHz (const std::vector<float>& x)
    {
        auto mag = tu::magnitudeSpectrum (x);
        const double binHz = kSR / double (x.size());
        double num = 0.0, den = 0.0;
        for (std::size_t k = 1; k < mag.size(); ++k) { num += double (k) * binHz * mag[k]; den += mag[k]; }
        return den > 0.0 ? num / den : 0.0;
    }
}

// ---------------------------------------------------------------------------
// 1. Sideband proof: sine carrier + sine modulator, ratio 2:1, moderate depth.
//    PM makes symmetric sidebands at fc +/- n*fmod. (fc/fmod chosen to land on bins.)
// ---------------------------------------------------------------------------
TEST_CASE ("FM: sine carrier + sine modulator makes symmetric sidebands at fc +/- n*fmod", "[dsp][fm][sideband]")
{
    const int n = 1 << 15;
    const double binHz = kSR / (double) n;
    const double fmod  = 100.0 * binHz;   // exact bins
    const double fc    = 350.0 * binHz;   // ratio 3.5:1 (keeps fc-2*fmod off DC)
    // Depth 0.2 -> index beta ~= 1.26: carrier stays dominant (J0), with clearly-present
    // sideband pairs (J1, J2). (At higher beta the carrier nulls, which is correct FM but a
    // poor fixture — that behaviour is exercised by the depth-broadening test below.)
    auto db = spectrumDb (renderPM (PolyBlepOscillator::Wave::Sine, fc, fmod, 0.2f, n));

    // Carrier is the dominant partial; first two sideband pairs are clearly present.
    REQUIRE (peakNearDb (db, fc,          n) > -1.0);
    REQUIRE (peakNearDb (db, fc + fmod,   n) > -6.0);
    REQUIRE (peakNearDb (db, fc - fmod,   n) > -6.0);
    REQUIRE (peakNearDb (db, fc + 2*fmod, n) > -20.0);
    REQUIRE (peakNearDb (db, fc - 2*fmod, n) > -20.0);

    // Sidebands are symmetric about the carrier (sine-modulator PM), within a few dB.
    REQUIRE (std::abs (peakNearDb (db, fc + fmod, n)   - peakNearDb (db, fc - fmod, n))   < 3.0);
    REQUIRE (std::abs (peakNearDb (db, fc + 2*fmod, n) - peakNearDb (db, fc - 2*fmod, n)) < 3.0);

    // A bin far from any fc +/- n*fmod partial (and away from DC) is spurious/alias: keep it low.
    REQUIRE (peakNearDb (db, fc + 0.5 * fmod, n, 1) < -40.0);
}

// ---------------------------------------------------------------------------
// 2. Depth 0 is bit-identical to the un-modulated oscillator (goldens frozen).
// ---------------------------------------------------------------------------
TEST_CASE ("FM: depth 0 is bit-identical to a plain oscillator", "[dsp][fm][golden]")
{
    const int n = 4096;
    for (auto w : { PolyBlepOscillator::Wave::Sine, PolyBlepOscillator::Wave::Triangle,
                    PolyBlepOscillator::Wave::Saw,  PolyBlepOscillator::Wave::Square })
    {
        auto fm0 = renderPM (w, 220.0, 110.0, 0.0f, n);   // depth 0 -> offset always 0.0

        PolyBlepOscillator plain; plain.prepare (kSR); plain.setWave (w); plain.setFrequency (220.0);
        for (int i = 0; i < n; ++i)
            REQUIRE (fm0[(std::size_t) i] == plain.nextSample());   // exact
    }
}

// ---------------------------------------------------------------------------
// 3. More depth = broader spectrum (more sideband energy => higher centroid).
// ---------------------------------------------------------------------------
TEST_CASE ("FM: increasing depth broadens the spectrum", "[dsp][fm]")
{
    const int n = 1 << 15;
    auto c = [n] (float d) { return centroidHz (renderPM (PolyBlepOscillator::Wave::Sine, 300.0, 300.0, d, n)); };
    const double c0 = c (0.0f), c1 = c (0.3f), c2 = c (0.9f);
    INFO ("centroids: " << c0 << " " << c1 << " " << c2);
    REQUIRE (c1 > c0 * 1.2);   // FM adds harmonics above the carrier
    REQUIRE (c2 > c1 * 1.2);   // ...and more depth adds more
}

// ---------------------------------------------------------------------------
// 4. Inharmonic ratio makes inharmonic partials (bell). Sane + bounded.
// ---------------------------------------------------------------------------
TEST_CASE ("FM: an inharmonic ratio (3.37:1) produces inharmonic partials, bounded", "[dsp][fm]")
{
    const int n = 1 << 15;
    const double fc = 440.0, fmod = fc / 3.37;
    auto sig = renderPM (PolyBlepOscillator::Wave::Sine, fc, fmod, 0.5f, n);
    REQUIRE (tu::allFinite (sig));
    REQUIRE (tu::peak (sig) <= 1.0001f);            // a sine carrier under PM never clips

    auto db = spectrumDb (sig);
    // The first inharmonic sidebands sit at fc +/- fmod (not on a harmonic of fc).
    REQUIRE (peakNearDb (db, fc + fmod, n) > -20.0);
    REQUIRE (peakNearDb (db, fc - fmod, n) > -20.0);
}

// ---------------------------------------------------------------------------
// 5. Keytrack: a fixed ratio scales the whole sideband structure with the note.
// ---------------------------------------------------------------------------
TEST_CASE ("FM: sidebands keytrack when the ratio is fixed", "[dsp][fm][keytrack]")
{
    const int n = 1 << 15;
    const double binHz = kSR / (double) n;
    const double fmodLo = 100.0 * binHz, fcLo = 200.0 * binHz;   // ratio 2:1
    auto dbLo = spectrumDb (renderPM (PolyBlepOscillator::Wave::Sine, fcLo,       fmodLo,       0.5f, n));
    auto dbHi = spectrumDb (renderPM (PolyBlepOscillator::Wave::Sine, fcLo * 2.0, fmodLo * 2.0, 0.5f, n));   // octave up

    // Lower note: sideband at fcLo + fmodLo. Higher note: it doubles to 2*(fcLo + fmodLo).
    REQUIRE (peakNearDb (dbLo, fcLo + fmodLo,             n) > -18.0);
    REQUIRE (peakNearDb (dbHi, 2.0 * (fcLo + fmodLo),     n) > -18.0);
    // ...and at the higher note the LOW-note sideband frequency is no longer a strong partial.
    REQUIRE (peakNearDb (dbHi, fcLo + fmodLo, n, 1) < -24.0);
}

// ---------------------------------------------------------------------------
// 6. Aliasing / safety: high note + max depth, sine and WT/tri carriers stay
//    finite + bounded (a sine carrier under PM is intrinsically |.|<=1).
// ---------------------------------------------------------------------------
TEST_CASE ("FM: high note + max depth stays finite and bounded", "[dsp][fm][aliasing]")
{
    const int n = 1 << 14;
    for (auto w : { PolyBlepOscillator::Wave::Sine, PolyBlepOscillator::Wave::Triangle })
        for (auto q : { PolyBlepOscillator::Quality::Efficient, PolyBlepOscillator::Quality::HQ })
        {
            auto sig = renderPM (w, 2637.0 /* ~E7 */, 2637.0, 1.0f, n, q);   // ratio 1:1, max depth
            REQUIRE (tu::allFinite (sig));
            REQUIRE (tu::peak (sig) <= 1.2f);      // triangle PM can graze past 1 by interpolation; well bounded
        }
}

// ---------------------------------------------------------------------------
// 7. Through the engine: the chain changes timbre even with an INAUDIBLE
//    modulator (osc2 level 0), and the SIN/TRI/WT carrier restriction holds.
// ---------------------------------------------------------------------------
TEST_CASE ("FM: engine chain runs a level-0 modulator; saw carrier is inert", "[dsp][fm][engine]")
{
    // Sine carrier (osc1), sine modulator (osc2) AT LEVEL 0, ratio 1:1.
    VoiceParams base;
    base.osc1Wave = 3 /*Sine*/; base.osc2Wave = 3 /*Sine*/; base.osc3Wave = 3;
    base.osc1Level = 0.8f; base.osc2Level = 0.0f; base.osc3Level = 0.0f;
    base = openFilter (base);

    VoiceParams dry = base;                 // FM off
    VoiceParams wet = base; wet.osc1Fm = 0.7f;   // osc2 (silent) modulates osc1

    const double cDry = centroidHz (renderVoice (dry));
    const double cWet = centroidHz (renderVoice (wet));
    INFO ("centroid dry=" << cDry << " wet=" << cWet);
    REQUIRE (cWet > cDry * 1.3);             // the inaudible modulator still shaped the carrier

    // Carrier restriction: a SAW carrier must ignore FM entirely -> bit-identical to FM off.
    VoiceParams sawDry = base; sawDry.osc1Wave = 0 /*Saw*/;
    VoiceParams sawWet = sawDry; sawWet.osc1Fm = 0.7f;
    auto a = renderVoice (sawDry);
    auto b = renderVoice (sawWet);
    REQUIRE (a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i)
        REQUIRE (a[i] == b[i]);              // FM inert on a saw carrier (exact)
}
