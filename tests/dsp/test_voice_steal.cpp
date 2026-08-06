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
