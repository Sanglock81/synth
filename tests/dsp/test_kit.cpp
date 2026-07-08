// ============================================================================
// Kit-part engine tests (Sub-phase 1). Per-pad param selection through the
// paramsFor(part, slot) seam, unmapped-trigger silence, sounding-note decoupling,
// chord pads, choke groups (in / cross / same-pad), and pad-level gain math.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "SynthEngine.h"
#include "test_util.h"
#include <vector>
#include <cmath>

namespace
{
    constexpr double kSR = 48000.0;

    // A vel-independent voice on one oscillator; short-ish so tests settle.
    VoiceParams voice (int wave, float sustain = 0.9f)
    {
        VoiceParams p;
        p.osc1Wave = wave; p.osc2Wave = wave;
        p.osc1Level = 0.8f; p.osc2Level = 0.0f; p.osc3Level = 0.0f; p.noiseLevel = 0.0f;
        p.cutoffHz = 18000.0f; p.resonance = 0.0f; p.filterEnvAmt = 0.0f; p.keytrack = 0.0f;
        p.velToAmp = 0.0f; p.velToCutoff = 0.0f;
        p.ampA = 0.002f; p.ampD = 0.02f; p.ampS = sustain; p.ampR = 0.02f;
        return p;
    }

    std::vector<float> render (SynthEngine& e, int n)
    {
        std::vector<float> out (n, 0.0f);
        e.render (out.data(), n, VoiceParams{}, 2.0f, 0, 0.0f, 0);
        return out;
    }

    double bandEnergy (const std::vector<float>& x, double loHz, double hiHz)
    {
        std::vector<float> w (8192, 0.0f);
        for (int i = 0; i < 8192 && i < (int) x.size(); ++i) w[(std::size_t) i] = x[(std::size_t) i];
        auto mag = tu::magnitudeSpectrum (w);
        const double binHz = kSR / 8192.0;
        double acc = 0.0;
        for (std::size_t k = 1; k < mag.size(); ++k) { const double f = k * binHz; if (f >= loHz && f < hiHz) acc += mag[k] * mag[k]; }
        return acc;
    }
    double noteHz (int n) { return 440.0 * std::pow (2.0, (n - 69) / 12.0); }
}

TEST_CASE ("kit: each pad renders with its OWN baked params (per-note seam)", "[kit][params]")
{
    SynthEngine e; e.prepare (kSR); e.setMaxVoices (16);

    KitData kit; kit.isKit = true;
    kit.pads[0] = { 36, { 60, 0, 0, 0 }, 1, 0 }; kit.params[0] = voice (3);   // sine
    kit.pads[1] = { 38, { 60, 0, 0, 0 }, 1, 0 }; kit.params[1] = voice (0);   // saw (same sounding note!)
    e.setPartKit (1, kit);
    REQUIRE (e.partIsKit (1));

    e.kitNoteOn (1, 36, 1.0f); auto sine = render (e, 8000); e.kitNoteOff (1, 36); render (e, 4000);
    e.kitNoteOn (1, 38, 1.0f); auto saw  = render (e, 8000);

    // Same sounding note (60), but pad 1 is a saw -> far more high-harmonic energy.
    const double sineHi = bandEnergy (sine, 1500.0, 12000.0);
    const double sawHi  = bandEnergy (saw,  1500.0, 12000.0);
    INFO ("sineHi=" << sineHi << " sawHi=" << sawHi);
    REQUIRE (sawHi > sineHi * 8.0);
}

TEST_CASE ("kit: a trigger with no pad is silent", "[kit][params]")
{
    SynthEngine e; e.prepare (kSR); e.setMaxVoices (16);
    KitData kit; kit.isKit = true;
    kit.pads[0] = { 36, { 60, 0, 0, 0 }, 1, 0 }; kit.params[0] = voice (3);
    e.setPartKit (1, kit);

    e.kitNoteOn (1, 99, 1.0f);                        // no pad maps trigger 99
    REQUIRE (tu::rms (render (e, 4000)) < 1.0e-6);
    REQUIRE (e.activeVoiceCount() == 0);
}

TEST_CASE ("kit: sounding note decouples pitch from the trigger", "[kit][pitch]")
{
    SynthEngine e; e.prepare (kSR); e.setMaxVoices (16);
    KitData kit; kit.isKit = true;
    kit.pads[0] = { 36, { 72, 0, 0, 0 }, 1, 0 }; kit.params[0] = voice (3);   // trigger 36 SOUNDS 72
    e.setPartKit (1, kit);

    e.kitNoteOn (1, 36, 1.0f);
    auto out = render (e, 8000);
    const double hz = tu::zeroCrossHz (out, 1000, 6000, kSR);
    INFO ("measured " << hz << " want " << noteHz (72));
    REQUIRE (hz == Catch::Approx (noteHz (72)).epsilon (0.03));   // sounds 72, not 36
}

TEST_CASE ("kit: a chord pad fires + releases all its notes under interleaving", "[kit][chordpad]")
{
    SynthEngine e; e.prepare (kSR); e.setMaxVoices (16);
    KitData kit; kit.isKit = true;
    kit.pads[0] = { 36, { 60, 64, 67, 0 }, 3, 0 }; kit.params[0] = voice (3);   // 3-note stab
    kit.pads[1] = { 38, { 48, 0, 0, 0 }, 1, 0 };    kit.params[1] = voice (3);
    e.setPartKit (1, kit);

    e.kitNoteOn (1, 36, 1.0f); render (e, 1000);
    REQUIRE (e.activeVoiceCount() == 3);                  // the whole triad
    e.kitNoteOn (1, 38, 1.0f); render (e, 1000);          // interleave another pad
    REQUIRE (e.activeVoiceCount() == 4);

    e.kitNoteOff (1, 36); render (e, 12000);              // release only the stab
    REQUIRE (e.activeVoiceCount() == 1);                  // pad 38 still held
}

TEST_CASE ("kit: choke groups (in-group chokes, cross-group doesn't, same-pad retriggers)", "[kit][choke]")
{
    auto build = [] (int groupA, int groupB)
    {
        KitData kit; kit.isKit = true;
        kit.pads[0] = { 46, { 46, 0, 0, 0 }, 1, groupA }; kit.params[0] = voice (3, 1.0f);   // open hat
        kit.pads[1] = { 42, { 42, 0, 0, 0 }, 1, groupB }; kit.params[1] = voice (3, 1.0f);   // closed hat
        return kit;
    };

    SECTION ("same group -> closed chokes open")
    {
        SynthEngine e; e.prepare (kSR); e.setMaxVoices (16);
        e.setPartKit (1, build (1, 1));
        e.kitNoteOn (1, 46, 1.0f); render (e, 2000);
        REQUIRE (e.activeVoiceCount() == 1);
        e.kitNoteOn (1, 42, 1.0f); render (e, 800);       // > 5 ms quick-release
        REQUIRE (e.activeVoiceCount() == 1);              // open hat was choked away
    }
    SECTION ("different groups -> both ring")
    {
        SynthEngine e; e.prepare (kSR); e.setMaxVoices (16);
        e.setPartKit (1, build (1, 2));
        e.kitNoteOn (1, 46, 1.0f); render (e, 2000);
        e.kitNoteOn (1, 42, 1.0f); render (e, 800);
        REQUIRE (e.activeVoiceCount() == 2);              // no choke across groups
    }
    SECTION ("same pad re-hit retriggers one voice")
    {
        SynthEngine e; e.prepare (kSR); e.setMaxVoices (16);
        e.setPartKit (1, build (1, 1));
        e.kitNoteOn (1, 46, 1.0f); render (e, 2000);
        e.kitNoteOn (1, 46, 1.0f); render (e, 800);
        REQUIRE (e.activeVoiceCount() == 1);              // monophonic pad
    }
}

TEST_CASE ("kit: pad level scales output (gain math)", "[kit][level]")
{
    auto rmsForGain = [] (float g, int note)
    {
        SynthEngine e; e.prepare (kSR); e.setMaxVoices (16);
        KitData kit; kit.isKit = true;
        auto vp = voice (3); vp.gain = g;
        kit.pads[0] = { 36, { note, 0, 0, 0 }, 1, 0 }; kit.params[0] = vp;
        e.setPartKit (1, kit);
        e.kitNoteOn (1, 36, 1.0f);
        return tu::rms (render (e, 8000));
    };
    const double full = rmsForGain (1.0f, 60);
    const double half = rmsForGain (0.5f, 62);
    INFO ("full=" << full << " half=" << half);
    REQUIRE (full > 0.01);
    REQUIRE (half == Catch::Approx (full * 0.5).epsilon (0.05));   // linear pad level
}

TEST_CASE ("kit: choke torture stays finite, bounded and click-free", "[kit][choke][torture]")
{
    SynthEngine e; e.prepare (kSR); e.setMaxVoices (16);
    KitData kit; kit.isKit = true;
    // Sine pads (smooth) so a hard-edge choke click would stand out from the waveform.
    kit.pads[0] = { 46, { 46, 0, 0, 0 }, 1, 1 }; kit.params[0] = voice (3, 1.0f);   // open hat, sustains
    kit.pads[1] = { 42, { 42, 0, 0, 0 }, 1, 1 }; kit.params[1] = voice (3, 1.0f);   // closed hat, same group
    e.setPartKit (1, kit);

    std::vector<float> out;
    for (int hit = 0; hit < 64; ++hit)
    {
        e.kitNoteOn (1, (hit & 1) ? 42 : 46, 1.0f);      // alternate -> each chokes the other
        std::vector<float> chunk (120, 0.0f);
        e.render (chunk.data(), 120, VoiceParams{}, 2.0f, 0, 0.0f, 0);
        out.insert (out.end(), chunk.begin(), chunk.end());
    }

    REQUIRE (tu::allFinite (out));
    REQUIRE (tu::peak (out) <= 1.0f);
    float maxJump = 0.0f;                                 // click detector: bounded sample-to-sample delta
    for (std::size_t i = 1; i < out.size(); ++i) maxJump = std::max (maxJump, std::abs (out[i] - out[i - 1]));
    INFO ("maxJump=" << maxJump);
    REQUIRE (maxJump < 0.05f);                            // no hard-edge click from the choke (smooth sines)
}

TEST_CASE ("kit: clearPartKit returns the part to a plain locked part", "[kit][params]")
{
    SynthEngine e; e.prepare (kSR); e.setMaxVoices (16);
    KitData kit; kit.isKit = true;
    kit.pads[0] = { 36, { 60, 0, 0, 0 }, 1, 0 }; kit.params[0] = voice (3);
    e.setPartKit (1, kit);
    REQUIRE (e.partIsKit (1));
    e.clearPartKit (1);
    REQUIRE_FALSE (e.partIsKit (1));
}
