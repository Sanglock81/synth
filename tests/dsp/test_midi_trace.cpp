// Synth — Copyright © 2026 John L Farmer. Licensed under AGPLv3; see LICENSE.
// ============================================================================
// G1.2 — MIDI/voice/looper trace. Verifies the env-gated tracer captures the right
// events (and NOTHING when disabled). Deterministic: uses the synchronous drain() +
// the setEnabled() test hook, so no background thread / file / timing is involved.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "SynthEngine.h"          // transitively includes ../Observability/MidiTracer.h
#include "Looper.h"
#include <vector>

namespace
{
    std::vector<mtrace::Event> drainAll()
    {
        std::vector<mtrace::Event> ev;
        mtrace::tracer().drain ([&] (const mtrace::Event& e) { ev.push_back (e); });
        return ev;
    }
    int countKind (const std::vector<mtrace::Event>& ev, mtrace::Ev k)
    {
        int c = 0; for (auto& e : ev) if (e.kind == k) ++c; return c;
    }
}

TEST_CASE ("midi trace: engine voice events captured when enabled", "[trace][observability]")
{
    auto& tr = mtrace::tracer();
    tr.setEnabled (true);
    drainAll();                                   // discard any residue from a prior case

    SynthEngine eng; eng.prepare (48000.0, 128); eng.setMaxVoices (8);
    eng.noteOn  (60, 0.8f, /*part*/ 1, 0, /*generator*/ false);   // live
    eng.noteOn  (64, 0.8f, /*part*/ 1, 0, /*generator*/ true);    // generator
    eng.noteOff (60, /*part*/ 1);
    eng.allNotesOff();

    const auto ev = drainAll();
    tr.setEnabled (false);

    REQUIRE (countKind (ev, mtrace::Ev::NoteOnLive)  >= 1);
    REQUIRE (countKind (ev, mtrace::Ev::NoteOnGen)   >= 1);
    REQUIRE (countKind (ev, mtrace::Ev::NoteOff)     >= 1);
    REQUIRE (countKind (ev, mtrace::Ev::AllNotesOff) == 1);

    // The live note-on carries (note=60, part=1) — the fields are wired correctly.
    bool found = false;
    for (auto& e : ev) if (e.kind == mtrace::Ev::NoteOnLive && e.a == 60 && e.c == 1) found = true;
    REQUIRE (found);
}

TEST_CASE ("midi trace: disabled -> zero events (the zero-overhead path)", "[trace][observability]")
{
    auto& tr = mtrace::tracer();
    tr.setEnabled (false);
    drainAll();                                   // clear

    SynthEngine eng; eng.prepare (48000.0, 128);
    for (int i = 0; i < 20; ++i) eng.noteOn (60 + i % 12, 0.7f, 0, 0, false);

    REQUIRE (drainAll().empty());                 // nothing was pushed while disabled
}

TEST_CASE ("midi trace: looper record + wrap + playback captured", "[trace][looper][observability]")
{
    auto& tr = mtrace::tracer();
    tr.setEnabled (true);
    drainAll();

    Looper lp;
    lp.setMasterLength (1000);
    lp.setLoopLength (0, 1000);
    lp.setRecording (0, true);
    lp.recordNote (0, 0,  60, 0.9f, true);        // note-on at t=0
    lp.recordNote (0, 10, 60, 0.0f, false);       // note-off at t=10
    lp.setRecording (0, false);
    lp.setPlaying (0, true);
    lp.advance (1000);                            // wrap -> arm the lane's events
    int emitted = 0;
    lp.playBlock (64, [&] (int, int, float, bool) { ++emitted; });   // window [0,64) catches both

    const auto ev = drainAll();
    tr.setEnabled (false);

    REQUIRE (countKind (ev, mtrace::Ev::LoopRec)  == 2);   // both recorded events traced
    REQUIRE (countKind (ev, mtrace::Ev::LoopWrap) >= 1);   // the wrap/arm traced
    REQUIRE (countKind (ev, mtrace::Ev::LoopEmit) == emitted);   // every playback emit traced
    REQUIRE (emitted == 2);
}
