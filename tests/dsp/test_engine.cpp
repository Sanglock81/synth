// ============================================================================
// Voice/Engine tests: determinism, note lifecycle, silence, and 17-on-16
// oldest-note stealing without an audible click.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
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
        p.oscMix = 0.0f; p.noiseLevel = 0.0f;
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
