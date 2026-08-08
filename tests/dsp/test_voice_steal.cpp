// ============================================================================
// Voice-steal priority (per-part isolation): a generator (seq/arp/looper) voice is
// always stolen before a live-played voice, so a running sequencer can never cut a
// note you play live. Live-vs-live stealing stays within the part.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "SynthEngine.h"

TEST_CASE ("voice steal: generators yield to live playing", "[engine][voices][isolation]")
{
    SynthEngine eng;
    eng.prepare (48000.0, 128);
    eng.setMaxVoices (16);

    // Fill the whole pool with GENERATOR notes on part 2 (as a running sequencer would).
    for (int i = 0; i < 16; ++i) eng.noteOn (40 + i, 0.9f, /*part*/ 2, /*slot*/ 0, /*generator*/ true);
    REQUIRE (eng.activeVoiceCount() == 16);
    REQUIRE (eng.activeVoiceCountForPart (2) == 16);

    // Play 6 LIVE notes on part 1 into the full pool.
    for (int i = 0; i < 6; ++i) eng.noteOn (60 + i, 0.9f, /*part*/ 1, /*slot*/ 0, /*generator*/ false);

    // All 6 live notes must be sounding — they stole generator voices, not each other.
    REQUIRE (eng.activeVoiceCountForPart (1) == 6);
    REQUIRE (eng.activeVoiceCountForPart (2) == 10);   // the sequencer yielded 6 voices
    REQUIRE (eng.activeVoiceCount() == 16);
}

TEST_CASE ("voice steal: live notes are never stolen by a generator", "[engine][voices][isolation]")
{
    SynthEngine eng;
    eng.prepare (48000.0, 128);
    eng.setMaxVoices (16);

    // A live chord on part 1 first.
    for (int i = 0; i < 6; ++i) eng.noteOn (60 + i, 0.9f, /*part*/ 1, /*slot*/ 0, /*generator*/ false);
    // Then a generator (seq) hammers part 2 with far more notes than fit.
    for (int i = 0; i < 30; ++i) eng.noteOn (36 + (i % 12), 0.9f, /*part*/ 2, /*slot*/ 0, /*generator*/ true);

    // The 6 live notes survive; the generator only ever stole generator voices.
    REQUIRE (eng.activeVoiceCountForPart (1) == 6);
}

TEST_CASE ("voice pool: default pool holds 24 simultaneous voices for multitimbral", "[engine][voices]")
{
    SynthEngine eng;
    eng.prepare (48000.0, 128);
    // No setMaxVoices call -> the default pool (raised 16 -> 24 for seq/kit/looper/lead split).

    // Spread 24 live notes across four parts (6 each) -> all must sound, no early steal.
    for (int part = 0; part < 4; ++part)
        for (int i = 0; i < 6; ++i)
            eng.noteOn (48 + part * 6 + i, 0.9f, part, /*slot*/ 0, /*generator*/ false);

    REQUIRE (eng.activeVoiceCount() == 24);
    for (int part = 0; part < 4; ++part)
        REQUIRE (eng.activeVoiceCountForPart (part) == 6);

    // The 25th live note steals the global-oldest (pool is full at 24, never grows).
    eng.noteOn (84, 0.9f, /*part*/ 0, /*slot*/ 0, /*generator*/ false);
    REQUIRE (eng.activeVoiceCount() == 24);
}

// #142 (F12 coverage): the live voice count must break out by ORIGIN so a leak's source is visible.
// LIVE = played voices, GEN = generator voices (arp/seq/looper), SMP = the separate sample-pad pool
// (which activeVoiceCount() does NOT include). This is what the F12 overlay reads live per block.
TEST_CASE ("voice breakdown: LIVE / GEN / SMP counted separately (#142)", "[engine][voices]")
{
    SynthEngine eng; eng.prepare (48000.0, 128); eng.setMaxVoices (16);
    int live = -1, gen = -1, smp = -1;
    eng.activeVoiceBreakdown (live, gen, smp);
    REQUIRE (live == 0); REQUIRE (gen == 0); REQUIRE (smp == 0);

    eng.noteOn (60, 0.9f, 0, 0, /*generator*/ false);   // a LIVE (played) note
    eng.noteOn (64, 0.9f, 0, 0, /*generator*/ true);    // a GENERATOR (arp/seq/looper) note
    eng.noteOn (67, 0.9f, 0, 0, /*generator*/ true);    // another generator note
    eng.activeVoiceBreakdown (live, gen, smp);
    REQUIRE (live == 1);                                  // one live voice
    REQUIRE (gen  == 2);                                  // two generator voices
    REQUIRE (smp  == 0);                                  // no sample-pad voices
    REQUIRE (live + gen == eng.activeVoiceCount());       // the synth breakdown reconciles with the total
}

// #144 PANIC: allNotesOff() must RELEASE every voice — live AND generator (and the sample pool) —
// via the envelope release path (click-safe), so the pool frees up. Here: 4 live + 4 generator
// voices on part 0, panic, then render past a short release and confirm the pool is empty.
TEST_CASE ("allNotesOff releases every voice, live + generator (#144)", "[engine][voices][panic]")
{
    SynthEngine eng; eng.prepare (48000.0, 128); eng.setMaxVoices (16);
    for (int i = 0; i < 4; ++i) eng.noteOn (60 + i, 0.9f, 0, 0, /*generator*/ false);
    for (int i = 0; i < 4; ++i) eng.noteOn (48 + i, 0.9f, 0, 0, /*generator*/ true);
    REQUIRE (eng.activeVoiceCount() == 8);

    eng.allNotesOff();                                   // PANIC (release path)
    VoiceParams p; p.osc1Wave = 0; p.osc1Level = 0.8f; p.ampS = 0.0f; p.ampR = 0.004f;   // short release
    std::vector<float> out (128, 0.0f);
    for (int b = 0; b < 300; ++b) eng.render (out.data(), 128, p, 3.0f, 0, 0.3f, 2);
    REQUIRE (eng.activeVoiceCount() == 0);               // every voice released and freed
}

// #141: a HELD chord tone must not be voice-stolen while a RELEASING (key-up, fading) voice exists.
// Repro of the reported "hold a chord, play higher notes, the held 3rd/5th vanish": hold a 3-note
// chord FIRST (the oldest voices), then fill the rest of the pool with NEWER key-up (releasing)
// voices, then play more notes into the full pool. The pre-fix oldest-own policy stole the OLDEST
// own-part voice — a held chord tone. The fix prefers releasing voices, so the chord survives.
TEST_CASE ("voice steal: held chord survives while releasing voices exist (#141)", "[engine][voices][steal]")
{
    auto& tr = mtrace::tracer();
    tr.setEnabled (true);
    tr.drain ([] (const mtrace::Event&) {});

    SynthEngine eng; eng.prepare (48000.0, 128); eng.setMaxVoices (8);

    const int chord[3] { 60, 64, 67 };                   // held, played FIRST -> the OLDEST voices
    for (int n : chord) eng.noteOn (n, 0.9f, 0);
    for (int i = 0; i < 5; ++i) eng.noteOn  (80 + i, 0.7f, 0);   // 5 NEWER voices...
    for (int i = 0; i < 5; ++i) eng.noteOff (80 + i, 0);         // ...released -> fading (still active)
    REQUIRE (eng.activeVoiceCount() == 8);               // pool full: 3 held + 5 releasing

    tr.drain ([] (const mtrace::Event&) {});             // discard the setup's events
    for (int n : { 72, 76, 79 }) eng.noteOn (n, 0.9f, 0);        // play into the full pool -> must steal

    std::vector<int> victims;
    tr.drain ([&] (const mtrace::Event& e) { if (e.kind == mtrace::Ev::VoiceSteal) victims.push_back (e.d); });
    tr.setEnabled (false);

    for (int n : chord)                                  // no held chord tone was ever the victim
    {
        bool stolen = false;
        for (int v : victims) if (v == n) stolen = true;
        REQUIRE_FALSE (stolen);
    }
}
