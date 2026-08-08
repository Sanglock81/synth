// ============================================================================
// Shared-transport clock alignment (task #53). The processor re-locks the sequencer and
// arpeggiator to the looper's bar downbeat at every bar boundary (realign), so seq step-1 and
// a fired arp step land on the looper boundary within block tolerance — at bar 1 and bar 100,
// and across a tempo change. (The arp's step realign keeps its RHYTHM on the grid but no longer
// resets the pattern index to 0 at the measure — see the arp free-run test.) This reproduces that
// owner logic at the DSP level.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "StepSequencer.h"
#include "Arpeggiator.h"
#include <vector>
#include <cmath>

TEST_CASE ("clock: seq step-1, arp downbeat, looper boundary align over 100 bars + tempo change", "[dsp][clock][align]")
{
    const int block = 128;

    StepSequencer seq;
    { StepSequencer::Config c; c.enabled = true; c.gate = 0.5f; c.swing = 0.15f;   // swing ON (self-accumulates)
      for (int s = 0; s < StepSequencer::kSteps; ++s) c.cells[0][s] = 0;
      c.cells[0][0] = 1; c.note[0] = 60;                                            // note 60 only on step 0
      c.samplesPerStep = 1000.0; seq.setConfig (c); }

    Arpeggiator arp;
    { Arpeggiator::Config a; a.enabled = true; a.mode = 0; a.samplesPerStep = 1000.0; for (auto& s : a.steps) s = 1.0f; arp.setConfig (a); }
    arp.noteOn (72, 0.9f);   // one held note -> the arp emits a note every step; the downbeat is the realign step

    // Shared transport: monotonic sample clock. barLen = 16 sixteenths. Loop = 4 bars.
    double samplesPerStep = 1000.0;
    auto barLenOf = [&] { return 16.0 * samplesPerStep; };
    const int N = 4;

    long long transport = 0;
    int prevBarIdx = -1;
    std::vector<long long> barBoundaries, seqStep0, arpDownbeat;

    auto runBars = [&] (int bars)
    {
        const long long target = transport + (long long) std::llround (bars * barLenOf());
        while (transport < target)
        {
            const double barLen = barLenOf();
            const long long loopLen = (long long) std::llround (N * barLen);
            const int loopPos = (int) (transport % loopLen);
            const int barIdx  = (int) (loopPos / barLen);
            if (barIdx != prevBarIdx)
            {
                // #145: exercise the PHASE-ONLY re-anchor (not the old force-fire). Guard 1: this
                // must still bound drift so seq step-0 / arp downbeat stay on the transport downbeat
                // over 100 bars + a tempo change (swing on, so per-bar drift is real).
                const double gp = transport / samplesPerStep;
                const long long fg = (long long) std::floor (gp);
                const int    tS = (int) (((fg % 16) + 16) % 16);
                const double tI = (gp - (double) fg) * samplesPerStep;
                seq.realignPhase (tS, tI); arp.realignPhase (tS, tI);
                prevBarIdx = barIdx;
                barBoundaries.push_back (transport);
            }
            const long long base = transport;   // block-start transport position
            seq.process (block, [&] (int off, int note, float, bool on) { if (on && note == 60) seqStep0.push_back (base + off); });
            arp.process (block, [&] (int off, int, float, bool on) { if (on && arp.enabled()) { if (arpDownbeat.empty() || base + off - arpDownbeat.back() > block) {} arpDownbeat.push_back (base + off); } });
            transport += block;
        }
    };

    runBars (50);
    samplesPerStep = 1500.0;   // TEMPO CHANGE mid-play (barLen grows); everything re-derives from the transport
    { StepSequencer::Config c; c.enabled = true; c.gate = 0.5f; c.swing = 0.15f;
      c.cells[0][0] = 1; c.note[0] = 60; c.samplesPerStep = 1500.0; seq.setConfig (c); }
    { Arpeggiator::Config a; a.enabled = true; a.mode = 0; a.samplesPerStep = 1500.0; for (auto& s : a.steps) s = 1.0f; arp.setConfig (a); }
    runBars (50);

    REQUIRE (barBoundaries.size() >= 100);

    // Every bar boundary has a seq step-0 note within block tolerance (the realign step).
    auto nearestWithin = [] (const std::vector<long long>& xs, long long t, int tol)
    { for (auto x : xs) if (std::llabs (x - t) <= tol) return true; return false; };

    int checked = 0;
    for (std::size_t b = 0; b < barBoundaries.size(); ++b)
    {
        const long long t = barBoundaries[b];
        REQUIRE (nearestWithin (seqStep0, t, block));       // seq step-1 lands on the downbeat
        REQUIRE (nearestWithin (arpDownbeat, t, block));    // arp downbeat coincides
        ++checked;
    }
    REQUIRE (checked >= 100);   // verified at bar 1 .. bar 100, across the tempo change
}

TEST_CASE ("arp: a bar realign keeps the rhythm on-grid but does NOT reset the pattern to step 0",
           "[dsp][clock][arp]")
{
    // The hands-on fix: striking a key mid-bar must not make the arp skip to its start at the next
    // bar. realign() re-locks the step phase (fires the next step on the downbeat) but advances the
    // pattern index normally rather than resetting it to 0.
    Arpeggiator arp;
    Arpeggiator::Config a; a.enabled = true; a.mode = 0; a.samplesPerStep = 1000.0;
    for (auto& s : a.steps) s = 1.0f;                 // all steps on
    arp.setConfig (a);
    arp.noteOn (60, 0.9f);

    const int block = 100;
    for (int i = 0; i < 55; ++i) arp.process (block, [] (int, int, float, bool) {});   // ~5.5 steps in
    const int before = arp.currentStep();
    REQUIRE (before > 0);                             // genuinely mid-pattern, not at step 0

    arp.realign();                                    // simulate the bar boundary
    int firedStep = -1;
    arp.process (block, [&] (int, int, float, bool on) { if (on && firedStep < 0) firedStep = arp.currentStep(); });
    REQUIRE (firedStep == (before + 1) % Arpeggiator::kNumSteps);   // pattern CONTINUED (not reset to 0)
    REQUIRE (firedStep != 0);                                       // and it did fire on the downbeat
}

TEST_CASE ("seq: enabling mid-bar picks up at the current grid step, not step 0 (task #127)",
           "[dsp][clock][resync]")
{
    // The hands-on fix: enabling the sequencer mid-bar must lock its pattern to the running grid
    // (step 0 stays on the bar downbeat) instead of firing step 0 at the enable instant and then
    // snapping back to 0 at the wrap. note 60 lives ONLY on step 0, so we can see exactly when the
    // pattern's step 0 comes round.
    StepSequencer seq;
    StepSequencer::Config c; c.enabled = true; c.gate = 0.5f; c.swing = 0.0f;
    for (int s = 0; s < StepSequencer::kSteps; ++s) c.cells[0][s] = 0;
    c.cells[0][0] = 1; c.note[0] = 60; c.samplesPerStep = 1000.0;
    seq.setConfig (c);

    // Enable at grid step 10 (10 sixteenths into the bar), a third of the way into that step.
    seq.startAtGrid (10, 300.0);
    REQUIRE (seq.currentStep() == 10);          // picked up at the beat the transport is on, NOT step 0

    // Run 8 steps (8000 samples) and record where step 0 (note 60) fires.
    std::vector<int> step0HitsAt; int t = 0; const int block = 250;
    for (int b = 0; b < 32; ++b)
    {
        seq.process (block, [&] (int off, int note, float, bool on) { if (on && note == 60) step0HitsAt.push_back (t + off); });
        t += block;
    }
    // Old behaviour fired step 0 at the enable instant (~t=0). Grid-anchored, the first step-0 hit
    // lands ~6 steps later at the bar downbeat (grid 16) — and only once in the 8-step window.
    REQUIRE (step0HitsAt.size() == 1);
    REQUIRE (step0HitsAt.front() > 4000);       // NOT at enable; out near the downbeat

    // Enabling exactly on a downbeat DOES fire step 0 immediately (the kick on the 1).
    StepSequencer seq2; seq2.setConfig (c);
    seq2.startAtGrid (0, 0.0);
    int firstNote = -1, firstAt = -1;
    seq2.process (128, [&] (int off, int note, float, bool on) { if (on && firstNote < 0) { firstNote = note; firstAt = off; } });
    REQUIRE (firstNote == 60);                  // step 0 fired
    REQUIRE (firstAt == 0);                     // right on the downbeat
}
