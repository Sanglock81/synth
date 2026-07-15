// ============================================================================
// Shared-transport clock alignment (task #53). The processor re-anchors the sequencer and
// arpeggiator to the looper's bar downbeat at every bar boundary (realign), so seq step-1,
// the arp downbeat, and the looper boundary coincide within block tolerance — at bar 1 and
// bar 100, and across a tempo change. This reproduces that owner logic at the DSP level.
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
                seq.realign(); arp.realign();
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
