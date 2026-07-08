// ============================================================================
// Diatonic chord engine (7B): grammar tables, modifiers, latest-wins, the note-off
// ledger under modifier churn, enable-toggle, and passthrough. Pure + JUCE-free.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "ChordEngine.h"
#include <vector>
#include <algorithm>

namespace
{
    using Vec = std::vector<int>;

    // Trigger tones for a note, then clear the ledger (keeps table checks isolated).
    Vec chord (ChordEngine& ce, int note)
    {
        int trig[4], rel[4]; int nt = 0, nr = 0;
        ce.noteOn (note, trig, nt, rel, nr);
        Vec v (trig, trig + nt);
        int r2[4]; int nr2 = 0; ce.noteOff (note, r2, nr2);
        std::sort (v.begin(), v.end());
        return v;
    }

    ChordEngine make (int root, int scale, bool enabled = true)
    {
        ChordEngine ce; ce.setRoot (root); ce.setScale (scale); ce.setEnabled (enabled);
        return ce;
    }
}

TEST_CASE ("C major: every diatonic triad", "[7b][chord][major]")
{
    auto ce = make (0, ChordEngine::Major);
    REQUIRE (chord (ce, 60) == Vec {60, 64, 67});   // C  E  G
    REQUIRE (chord (ce, 62) == Vec {62, 65, 69});   // D  F  A
    REQUIRE (chord (ce, 64) == Vec {64, 67, 71});   // E  G  B
    REQUIRE (chord (ce, 65) == Vec {65, 69, 72});   // F  A  C
    REQUIRE (chord (ce, 67) == Vec {67, 71, 74});   // G  B  D
    REQUIRE (chord (ce, 69) == Vec {69, 72, 76});   // A  C  E
    REQUIRE (chord (ce, 71) == Vec {71, 74, 77});   // B  D  F
}

TEST_CASE ("C major: diatonic sevenths (7TH held)", "[7b][chord][seventh]")
{
    auto ce = make (0, ChordEngine::Major);
    ce.setSeventhHeld (true);
    REQUIRE (chord (ce, 60) == Vec {60, 64, 67, 71});   // Cmaj7
    REQUIRE (chord (ce, 62) == Vec {62, 65, 69, 72});   // Dm7
    REQUIRE (chord (ce, 67) == Vec {67, 71, 74, 77});   // G7  (dominant, diatonic)
    REQUIRE (chord (ce, 71) == Vec {71, 74, 77, 81});   // Bm7b5 (half-diminished)
}

TEST_CASE ("A minor / E major / F minor: spot triads", "[7b][chord][keys]")
{
    { auto ce = make (9, ChordEngine::Minor);            // A natural minor
      REQUIRE (chord (ce, 69) == Vec {69, 72, 76});      // A  C  E
      REQUIRE (chord (ce, 71) == Vec {71, 74, 77});      // B  D  F
      REQUIRE (chord (ce, 72) == Vec {72, 76, 79}); }    // C  E  G

    { auto ce = make (4, ChordEngine::Major);            // E major
      REQUIRE (chord (ce, 64) == Vec {64, 68, 71});      // E  G# B
      REQUIRE (chord (ce, 66) == Vec {66, 69, 73}); }    // F# A  C#

    { auto ce = make (5, ChordEngine::Minor);            // F natural minor
      REQUIRE (chord (ce, 65) == Vec {65, 68, 72});      // F  Ab C
      REQUIRE (chord (ce, 67) == Vec {67, 70, 73}); }    // G  Bb Db
}

TEST_CASE ("out-of-scale note passes through as a single note", "[7b][chord][passthrough]")
{
    auto ce = make (0, ChordEngine::Major);
    REQUIRE (chord (ce, 61) == Vec {61});                // C# is not in C major
    REQUIRE (chord (ce, 66) == Vec {66});                // F#
}

TEST_CASE ("chord OFF is a bit-identical passthrough", "[7b][chord][disabled]")
{
    auto ce = make (0, ChordEngine::Major, /*enabled*/ false);
    REQUIRE (chord (ce, 60) == Vec {60});
    REQUIRE (chord (ce, 61) == Vec {61});
    REQUIRE (chord (ce, 67) == Vec {67});
}

TEST_CASE ("forcers: absolute-interval chords on the played note", "[7b][chord][forcer]")
{
    auto ce = make (0, ChordEngine::Major);
    auto force = [&] (ChordEngine::Forcer f) { ce.setForcerHeld (f, true); auto v = chord (ce, 60); ce.setForcerHeld (f, false); return v; };
    REQUIRE (force (ChordEngine::Maj)  == Vec {60, 64, 67});        // C major
    REQUIRE (force (ChordEngine::Min)  == Vec {60, 63, 67});        // C minor
    REQUIRE (force (ChordEngine::Sus4) == Vec {60, 65, 67});        // Csus4
    REQUIRE (force (ChordEngine::Sus2) == Vec {60, 62, 67});        // Csus2
    REQUIRE (force (ChordEngine::Dim)  == Vec {60, 63, 66});        // Cdim
    REQUIRE (force (ChordEngine::Dom7) == Vec {60, 64, 67, 70});    // C7 (always carries the b7)
}

TEST_CASE ("forcer + 7TH follows the forcer", "[7b][chord][forcer][seventh]")
{
    auto ce = make (0, ChordEngine::Major);
    ce.setSeventhHeld (true);
    auto force = [&] (ChordEngine::Forcer f) { ce.setForcerHeld (f, true); auto v = chord (ce, 60); ce.setForcerHeld (f, false); return v; };
    REQUIRE (force (ChordEngine::Min) == Vec {60, 63, 67, 70});     // Cm7
    REQUIRE (force (ChordEngine::Maj) == Vec {60, 64, 67, 71});     // Cmaj7
    REQUIRE (force (ChordEngine::Dim) == Vec {60, 63, 66, 69});     // Cdim7
}

TEST_CASE ("conflicting forcers: latest still-held wins", "[7b][chord][latest]")
{
    auto ce = make (0, ChordEngine::Major);
    ce.setForcerHeld (ChordEngine::Min, true);
    ce.setForcerHeld (ChordEngine::Maj, true);                      // Maj pressed later
    REQUIRE (chord (ce, 60) == Vec {60, 64, 67});                   // major wins
    ce.setForcerHeld (ChordEngine::Maj, false);                     // release Maj
    REQUIRE (chord (ce, 60) == Vec {60, 63, 67});                   // Min (still held) returns
    ce.setForcerHeld (ChordEngine::Min, false);
    REQUIRE (chord (ce, 60) == Vec {60, 64, 67});                   // back to diatonic C major
}

TEST_CASE ("ledger: note-off releases the note-ON tones despite modifier churn", "[7b][chord][ledger]")
{
    auto ce = make (0, ChordEngine::Major);
    ce.setForcerHeld (ChordEngine::Min, true);

    int trig[4], rel[4]; int nt = 0, nr = 0;
    ce.noteOn (60, trig, nt, rel, nr);                              // Cm: 60 63 67
    Vec on (trig, trig + nt); std::sort (on.begin(), on.end());
    REQUIRE (on == Vec {60, 63, 67});

    // Churn the modifiers WHILE held (no re-press): release Min, press Maj+7TH.
    ce.setForcerHeld (ChordEngine::Min, false);
    ce.setForcerHeld (ChordEngine::Maj, true);
    ce.setSeventhHeld (true);

    int roff[4]; int nroff = 0;
    ce.noteOff (60, roff, nroff);
    Vec off (roff, roff + nroff); std::sort (off.begin(), off.end());
    REQUIRE (off == Vec {60, 63, 67});                             // exactly what note-ON produced
}

TEST_CASE ("re-press releases only changed tones, keeps the rest (no stuck notes)", "[7b][chord][retrigger]")
{
    auto ce = make (0, ChordEngine::Major);
    int trig[4], rel[4]; int nt = 0, nr = 0;

    ce.setForcerHeld (ChordEngine::Min, true);
    ce.noteOn (60, trig, nt, rel, nr);                             // Cm 60 63 67, nothing to release
    REQUIRE (nr == 0);

    ce.setForcerHeld (ChordEngine::Min, false);
    ce.setForcerHeld (ChordEngine::Maj, true);
    ce.noteOn (60, trig, nt, rel, nr);                             // re-press -> CMaj 60 64 67
    Vec trigger (trig, trig + nt); std::sort (trigger.begin(), trigger.end());
    Vec release (rel, rel + nr);   std::sort (release.begin(), release.end());
    REQUIRE (trigger == Vec {60, 64, 67});
    REQUIRE (release == Vec {63});                                 // the minor 3rd left; 60/67 retrigger
}

TEST_CASE ("enable toggled mid-hold strands no notes", "[7b][chord][toggle]")
{
    auto ce = make (0, ChordEngine::Major);
    int trig[4], rel[4]; int nt = 0, nr = 0;
    ce.noteOn (60, trig, nt, rel, nr);                             // C E G held
    REQUIRE (nt == 3);

    ce.setEnabled (false);                                        // toggle OFF while held
    int roff[4]; int nroff = 0;
    ce.noteOff (60, roff, nroff);
    Vec off (roff, roff + nroff); std::sort (off.begin(), off.end());
    REQUIRE (off == Vec {60, 64, 67});                            // ledger still releases the chord
}
