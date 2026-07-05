// ============================================================================
// Mono / legato note priority + glide (portamento). Engine-level, JUCE-free.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "SynthEngine.h"
#include "test_util.h"
#include <vector>
#include <functional>

namespace
{
    constexpr double kSR = 48000.0;

    VoiceParams sineP (float glide = 0.0f)
    {
        VoiceParams p;
        p.osc1Wave = 3; p.osc2Wave = 3; p.oscMix = 0.0f; p.noiseLevel = 0.0f;
        p.cutoffHz = 18000.0f; p.resonance = 0.0f; p.filterEnvAmt = 0.0f;
        p.ampA = 0.002f; p.ampD = 0.01f; p.ampS = 1.0f; p.ampR = 0.05f;
        p.glideTime = glide;
        return p;
    }

    // Render `n` samples continuing an engine, return the buffer.
    std::vector<float> cont (SynthEngine& e, VoiceParams p, int n)
    {
        std::vector<float> out (n, 0.0f);
        e.render (out.data(), n, p, 2.0f, 0, 0.0f, 0);
        return out;
    }

    double midiHz (int n) { return 440.0 * std::pow (2.0, (n - 69) / 12.0); }
}

TEST_CASE ("mono mode: last-note priority with fallback", "[mono][priority]")
{
    SynthEngine e; e.prepare (kSR); e.setPolyMode (1);
    VoiceParams p = sineP();

    e.noteOn (60, 0.8f);
    auto a = cont (e, p, 8192);
    REQUIRE (tu::zeroCrossHz (a, 2048, 4096, kSR) == Catch::Approx (midiHz (60)).epsilon (0.03));

    e.noteOn (67, 0.8f);                         // both held -> newest sounds
    auto b = cont (e, p, 8192);
    REQUIRE (tu::zeroCrossHz (b, 2048, 4096, kSR) == Catch::Approx (midiHz (67)).epsilon (0.03));

    e.noteOff (67);                              // fall back to still-held 60
    auto c = cont (e, p, 8192);
    REQUIRE (tu::zeroCrossHz (c, 2048, 4096, kSR) == Catch::Approx (midiHz (60)).epsilon (0.03));

    e.noteOff (60);
    auto d = cont (e, p, int (kSR * 0.3));
    std::vector<float> tail (d.end() - 2048, d.end());
    REQUIRE (tu::peak (tail) < 1.0e-4f);         // all released -> silent
}

TEST_CASE ("legato does not retrigger the envelope; mono does", "[mono][legato]")
{
    // Long attack, low sustain. Play note 1, let it settle to sustain. Play note
    // 2 while holding. Legato: level stays ~sustain. Mono: envelope retriggers
    // (attacks upward from the current level), so level rises.
    auto levelAfterSecondNote = [](int mode)
    {
        SynthEngine e; e.prepare (kSR); e.setPolyMode (mode);
        VoiceParams p = sineP();
        p.ampA = 0.2f; p.ampD = 0.001f; p.ampS = 0.3f;      // slow attack, sustain 0.3

        e.noteOn (60, 1.0f);
        cont (e, p, int (kSR * 0.5));                        // settle to sustain
        auto before = cont (e, p, 2048);
        e.noteOn (64, 1.0f);                                 // second note, still holding 60
        cont (e, p, int (kSR * 0.03));                       // 30 ms into any re-attack
        auto after = cont (e, p, 2048);
        return std::make_pair (tu::rms (before), tu::rms (after));
    };

    auto [legBefore, legAfter] = levelAfterSecondNote (2);   // legato
    auto [monBefore, monAfter] = levelAfterSecondNote (1);   // mono

    INFO ("legato " << legBefore << "->" << legAfter << "  mono " << monBefore << "->" << monAfter);
    REQUIRE (legAfter == Catch::Approx (legBefore).epsilon (0.15));   // legato: ~unchanged
    REQUIRE (monAfter > monBefore * 1.3);                            // mono: retriggered upward
}

TEST_CASE ("glide slews pitch from old note to new over glide time", "[mono][glide]")
{
    SynthEngine e; e.prepare (kSR); e.setPolyMode (1);
    VoiceParams p = sineP (0.1f);                            // 100 ms glide

    e.noteOn (48, 0.9f);                                     // ~130 Hz
    cont (e, p, int (kSR * 0.2));                            // settle
    e.noteOn (60, 0.9f);                                     // jump target ~261 Hz

    // Each window is ~21 ms; with a 100 ms time constant the glide needs ~4-5
    // taus to arrive, so advance ~0.5 s before the final measurement.
    const double hEarly = tu::zeroCrossHz (cont (e, p, 1024), 0, 1024, kSR);   // ~0-21 ms
    const double hMid   = tu::zeroCrossHz (cont (e, p, 1024), 0, 1024, kSR);   // ~21-43 ms
    cont (e, p, int (kSR * 0.5));                                              // let it arrive
    const double hFinal = tu::zeroCrossHz (cont (e, p, 2048), 0, 2048, kSR);

    INFO ("glide Hz: early=" << hEarly << " mid=" << hMid << " final=" << hFinal);
    REQUIRE (hEarly > midiHz (48) * 1.02);        // moved off the start note
    REQUIRE (hEarly < midiHz (60) * 0.95);        // but not yet arrived
    REQUIRE (hMid   > hEarly);                     // rising toward target
    REQUIRE (hFinal == Catch::Approx (midiHz (60)).epsilon (0.03));   // arrived
}

TEST_CASE ("glide time 0 = instant pitch (no portamento)", "[mono][glide]")
{
    SynthEngine e; e.prepare (kSR); e.setPolyMode (1);
    VoiceParams p = sineP (0.0f);

    e.noteOn (48, 0.9f);
    cont (e, p, int (kSR * 0.2));
    e.noteOn (60, 0.9f);
    auto s = cont (e, p, 4096);
    REQUIRE (tu::zeroCrossHz (s, 512, 3072, kSR) == Catch::Approx (midiHz (60)).epsilon (0.03));
}
