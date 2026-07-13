// ============================================================================
// Voice/Engine tests: determinism, note lifecycle, silence, and 17-on-16
// oldest-note stealing without an audible click.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "SynthEngine.h"
#include "test_util.h"
#include <vector>
#include <cstring>
#include <functional>

namespace
{
    constexpr double kSR = 48000.0;

    struct Event { int sample; std::function<void(SynthEngine&)> fn; };

    // Render totalSamples, applying scheduled events at exact sample offsets.
    std::vector<float> renderScript (SynthEngine& e, VoiceParams p, int totalSamples,
                                     std::vector<Event> events)
    {
        std::vector<float> out (totalSamples, 0.0f);
        int cursor = 0;
        for (auto& ev : events)
        {
            if (ev.sample > cursor)
            {
                e.render (out.data() + cursor, ev.sample - cursor, p, 2.0f, 0, 0.0f, 0);
                cursor = ev.sample;
            }
            ev.fn (e);
        }
        if (cursor < totalSamples)
            e.render (out.data() + cursor, totalSamples - cursor, p, 2.0f, 0, 0.0f, 0);
        return out;
    }

    float maxDeltaRange (const std::vector<float>& x, int lo, int hi)
    {
        lo = std::max (lo, 1); hi = std::min (hi, int (x.size()));
        float d = 0.0f;
        for (int i = lo; i < hi; ++i) d = std::max (d, std::abs (x[i] - x[i - 1]));
        return d;
    }

    VoiceParams sineParams()
    {
        VoiceParams p;
        p.osc1Wave = 3; p.osc2Wave = 3;       // sine
        p.oscMix = 0.0f; p.noiseLevel = 0.0f; p.osc1Level = 0.8f; p.osc2Level = 0.0f;
        p.cutoffHz = 18000.0f; p.resonance = 0.0f; p.filterEnvAmt = 0.0f;
        p.ampA = 0.002f; p.ampD = 0.05f; p.ampS = 0.9f; p.ampR = 0.1f;
        return p;
    }
}

TEST_CASE ("engine render is deterministic (bit-exact across two runs)", "[engine][determinism]")
{
    auto run = []
    {
        SynthEngine e; e.prepare (kSR);
        VoiceParams p;
        return renderScript (e, p, 48000, {
            { 0,     [](SynthEngine& en){ en.noteOn (60, 0.8f); } },
            { 2000,  [](SynthEngine& en){ en.noteOn (64, 0.7f); } },
            { 4000,  [](SynthEngine& en){ en.noteOn (67, 0.6f); } },
            { 20000, [](SynthEngine& en){ en.noteOff (60); } },
        });
    };
    auto a = run();
    auto b = run();
    REQUIRE (a.size() == b.size());
    REQUIRE (std::memcmp (a.data(), b.data(), a.size() * sizeof (float)) == 0);
}

TEST_CASE ("silence in -> silence out when no notes are active", "[engine][silence]")
{
    SynthEngine e; e.prepare (kSR);
    VoiceParams p;
    std::vector<float> out (4096, 0.0f);
    e.render (out.data(), 4096, p, 2.0f, 0, 0.0f, 0);
    REQUIRE (tu::peak (out) == 0.0f);
}

TEST_CASE ("note-on/off lifecycle frees the voice (silence after release)", "[engine][lifecycle]")
{
    SynthEngine e; e.prepare (kSR);
    VoiceParams p; p.ampR = 0.1f;

    auto out = renderScript (e, p, int (kSR * 2.0), {
        { 0,               [](SynthEngine& en){ en.noteOn (60, 0.9f); } },
        { int (kSR * 0.5), [](SynthEngine& en){ en.noteOff (60); } },
    });

    // Last 0.5 s (well after release) must be silent -> voice deactivated.
    std::vector<float> tail (out.end() - int (kSR * 0.5), out.end());
    REQUIRE (tu::peak (tail) < 1.0e-4f);

    // And a fresh note still sounds -> the voice was reusable.
    std::vector<float> more (8192, 0.0f);
    e.noteOn (72, 0.9f);
    e.render (more.data(), 8192, p, 2.0f, 0, 0.0f, 0);
    REQUIRE (tu::peak (more) > 0.01f);
}

TEST_CASE ("setMaxVoices caps active polyphony (steals within the cap)", "[engine][voicecap]")
{
    SynthEngine e; e.prepare (kSR);
    e.setMaxVoices (4);
    VoiceParams p = sineParams();

    // Play 6 distinct notes; only 4 can sound. The 5th and 6th steal, and no
    // more than 4 notes are ever simultaneously audible. We can't query voice
    // count directly, so verify the engine stays finite/bounded and that after
    // releasing all, output goes silent (no leaked voices above the cap).
    std::vector<Event> ev;
    for (int i = 0; i < 6; ++i)
        ev.push_back ({ i * 8, [i](SynthEngine& en){ en.noteOn (48 + i * 2, 0.7f); } });
    for (int i = 0; i < 6; ++i)
        ev.push_back ({ 6000 + i * 8, [i](SynthEngine& en){ en.noteOff (48 + i * 2); } });

    auto out = renderScript (e, p, int (kSR * 0.4), ev);
    REQUIRE (tu::allFinite (out));
    REQUIRE (tu::peak (out) < 4.0f);
    std::vector<float> tail (out.end() - int (kSR * 0.1), out.end());
    REQUIRE (tu::peak (tail) < 1.0e-4f);          // all released -> silent
}

TEST_CASE ("retriggering a held note does not click", "[engine][retrigger]")
{
    SynthEngine e; e.prepare (kSR);
    VoiceParams p = sineParams();

    const int reAt = 12000;                            // retrigger point (past attack)
    auto out = renderScript (e, p, reAt + 4096, {
        { 0,    [](SynthEngine& en){ en.noteOn (60, 0.8f); } },
        { reAt, [](SynthEngine& en){ en.noteOn (60, 0.8f); } },   // same note again
    });

    const float pre  = maxDeltaRange (out, reAt - 2048, reAt - 64);
    const float atEv = maxDeltaRange (out, reAt - 8,    reAt + 512);
    INFO ("pre=" << pre << " at-retrigger=" << atEv);
    REQUIRE (tu::allFinite (out));
    REQUIRE (atEv <= pre + 0.05f);                     // no phase-reset discontinuity
}

TEST_CASE ("mod env -> pitch: +12 st is an octave, decaying to note pitch", "[engine][7a][envpitch]")
{
    auto renderNote = [] (float envToPitch, float fltS, float fltD)
    {
        SynthEngine e; e.prepare (kSR);
        VoiceParams p = sineParams();              // sine osc1, cutoff wide, filterEnvAmt 0
        p.fltEnvToPitch = envToPitch;
        p.fltA = 0.001f; p.fltD = fltD; p.fltS = fltS;                 // instant up
        p.ampA = 0.001f; p.ampD = 0.5f; p.ampS = 0.9f; p.ampR = 0.1f;  // audible throughout
        e.noteOn (69, 1.0f);                       // A4 = 440 Hz
        std::vector<float> out (int (kSR * 1.5), 0.0f);
        e.render (out.data(), (int) out.size(), p, 2.0f, 0, 0.0f, 0);
        return out;
    };

    // (1) Env HELD at peak (sustain 1.0): +12 st is exactly one octave -> 880 Hz.
    {
        auto out = renderNote (12.0f, 1.0f, 0.001f);
        const double hz = tu::zeroCrossHz (out, int (kSR * 0.05), int (kSR * 0.25), kSR);
        INFO ("held +12 -> " << hz << " Hz");
        REQUIRE (hz == Catch::Approx (880.0).margin (20.0));          // an octave up
    }

    // (2) Env DECAYS to 0: pitch starts high and settles at the note (440 Hz).
    {
        auto out = renderNote (12.0f, 0.0f, 0.12f);
        const double early = tu::zeroCrossHz (out, int (kSR * 0.005), int (kSR * 0.02), kSR);
        const double late  = tu::zeroCrossHz (out, int (kSR * 0.8),   int (kSR * 0.6),  kSR);
        INFO ("decaying early=" << early << " late=" << late);
        REQUIRE (early > 600.0);                                      // clearly pitched up at onset
        REQUIRE (late  == Catch::Approx (440.0).margin (10.0));       // lands at note pitch
    }

    // (3) Default 0 is inert (golden-safe): stays at 440 the whole time.
    {
        auto out = renderNote (0.0f, 0.0f, 0.12f);
        REQUIRE (tu::zeroCrossHz (out, int (kSR * 0.02), int (kSR * 0.4), kSR)
                 == Catch::Approx (440.0).margin (6.0));
    }
}

TEST_CASE ("locked part renders with its baked params, not the live part", "[engine][7c][parts]")
{
    VoiceParams live = sineParams();                 // part 0: sine at the note
    VoiceParams locked = sineParams(); locked.osc1Octave = 1.0f;   // part 1: an octave up

    auto pitchOf = [&] (int part)
    {
        SynthEngine e; e.prepare (kSR);
        e.setLockedPartParams (1, locked);           // publish the locked-part params
        e.noteOn (60, 1.0f, part);                    // play note 60 on `part`
        std::vector<float> out (int (kSR * 0.3), 0.0f);
        e.render (out.data(), (int) out.size(), live, 2.0f, 0, 0.0f, 0);
        return tu::zeroCrossHz (out, int (kSR * 0.05), int (kSR * 0.2), kSR);
    };

    REQUIRE (pitchOf (1) == Catch::Approx (523.25).margin (9.0));   // locked param: C5 (note+1 oct)
    REQUIRE (pitchOf (0) == Catch::Approx (261.63).margin (5.0));   // live param: C4
}

TEST_CASE ("voice-sum headroom trim is a fixed reference, independent of pool size", "[engine][bug4][headroom]")
{
    // The engine applies a fixed voice-sum trim (1/sqrt(kTrimVoices), kTrimVoices=16)
    // so a summing polysynth stays well under full-scale (Bug 4: unmanaged voice sums
    // clipped the DAC). The trim is a FIXED REFERENCE — deliberately DECOUPLED from the
    // pool size so raising maxVoices (16 -> 24 for the multitimbral split) never shifts a
    // single note's level or the render goldens. Same note at cap 4, 16, and 24 must
    // therefore all produce the SAME peak.
    auto renderOneNote = [] (int cap)
    {
        SynthEngine e; e.prepare (kSR); e.setMaxVoices (cap);
        VoiceParams p = sineParams();
        std::vector<float> out (8192, 0.0f);
        e.noteOn (60, 1.0f);
        e.render (out.data(), 8192, p, 2.0f, 0, 0.0f, 0);
        return tu::peak (out);
    };

    const float peak16 = renderOneNote (16);
    const float peak4  = renderOneNote (4);
    const float peak24 = renderOneNote (24);   // the raised pool
    REQUIRE (peak16 > 0.0f);
    REQUIRE (peak4  == Catch::Approx (peak16).epsilon (0.001));   // pool size doesn't change level
    REQUIRE (peak24 == Catch::Approx (peak16).epsilon (0.001));   // ...goldens stay bit-identical
}

TEST_CASE ("17 notes on 16 voices steals oldest without a click", "[engine][steal]")
{
    SynthEngine e; e.prepare (kSR);
    VoiceParams p = sineParams();

    // Fill all 16 voices, let them settle, then a 17th note steals the oldest.
    std::vector<Event> events;
    for (int i = 0; i < 16; ++i)
        events.push_back ({ i * 4, [i](SynthEngine& en){ en.noteOn (48 + i, 0.7f); } });

    const int stealAt = 20000;                         // well into steady state
    events.push_back ({ stealAt, [](SynthEngine& en){ en.noteOn (72, 0.7f); } });

    auto out = renderScript (e, p, stealAt + 4096, events);

    // Steady-state delta just before the steal vs. delta across the steal event.
    const float pre  = maxDeltaRange (out, stealAt - 2048, stealAt - 64);
    const float atEv = maxDeltaRange (out, stealAt - 8,    stealAt + 512);

    INFO ("pre-steal maxDelta=" << pre << "  at-steal maxDelta=" << atEv);
    REQUIRE (tu::allFinite (out));
    // The steal must not introduce a discontinuity beyond the steady-state edge.
    REQUIRE (atEv <= pre + 0.05f);
}
