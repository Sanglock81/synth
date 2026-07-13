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
