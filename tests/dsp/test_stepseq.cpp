// ============================================================================
// Multi-row step sequencer: rows fire their notes on their on-steps, mute silences
// a row, accent raises velocity, gate releases before the next step, and independent
// rows layer. JUCE-free (DSP-only).
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "StepSequencer.h"
#include <vector>

namespace
{
    struct Ev { int t; int note; float vel; bool on; };

    std::vector<Ev> run (StepSequencer& s, int totalSamples, int block)
    {
        std::vector<Ev> evs; int base = 0;
        for (int done = 0; done < totalSamples; done += block)
        {
            const int n = std::min (block, totalSamples - done);
            s.process (n, [&] (int off, int note, float vel, bool on) { evs.push_back ({ base + off, note, vel, on }); });
            base += n;
        }
        return evs;
    }

    StepSequencer::Config baseCfg()
    {
        StepSequencer::Config c;
        c.enabled = true; c.gate = 0.5f; c.swing = 0.0f; c.samplesPerStep = 100;
        return c;
    }
}

TEST_CASE ("stepseq disabled emits nothing", "[dsp][stepseq]")
{
    StepSequencer s; auto c = baseCfg(); c.enabled = false;
    c.cells[0].fill (StepSequencer::On); s.setConfig (c);
    REQUIRE (run (s, 800, 32).empty());
}

TEST_CASE ("stepseq fires a row's note on its on-steps", "[dsp][stepseq]")
{
    StepSequencer s; auto c = baseCfg();
    for (int st = 0; st < 16; ++st) c.cells[0][(std::size_t) st] = (st % 4 == 0) ? StepSequencer::On : StepSequencer::Off;   // 4-on-the-floor
    s.setConfig (c);
    auto e = run (s, 1550, 64);                    // just under one 16-step bar (no wrap re-fire)
    int ons = 0; for (auto& x : e) if (x.on) { ++ons; REQUIRE (x.note == 36); }
    REQUIRE (ons == 4);                            // steps 0,4,8,12
}

TEST_CASE ("stepseq layers independent rows", "[dsp][stepseq]")
{
    StepSequencer s; auto c = baseCfg();
    c.cells[0][0] = StepSequencer::On;             // kick on step 0
    c.cells[2][0] = StepSequencer::On;             // snare row on step 0 too
    s.setConfig (c);
    auto e = run (s, 200, 200);
    bool kick = false, snare = false;
    for (auto& x : e) if (x.on) { if (x.note == 36) kick = true; if (x.note == 38) snare = true; }
    REQUIRE (kick); REQUIRE (snare);
}

TEST_CASE ("stepseq mute silences a row", "[dsp][stepseq]")
{
    StepSequencer s; auto c = baseCfg();
    c.cells[0][0] = StepSequencer::On; c.mute[0] = true;
    c.cells[1][0] = StepSequencer::On;             // row 1 (note 37) still plays
    s.setConfig (c);
    auto e = run (s, 200, 200);
    for (auto& x : e) if (x.on) REQUIRE (x.note == 37);
}

TEST_CASE ("stepseq accent raises velocity", "[dsp][stepseq]")
{
    StepSequencer s; auto c = baseCfg();
    c.cells[0][0] = StepSequencer::On; c.cells[0][4] = StepSequencer::Accent;
    s.setConfig (c);
    auto e = run (s, 600, 300);
    float normal = 0, accent = 0;
    for (auto& x : e) if (x.on) { if (x.t < 50) normal = x.vel; else accent = std::max (accent, x.vel); }
    REQUIRE (accent > normal);
}

TEST_CASE ("stepseq gate releases before the next step", "[dsp][stepseq]")
{
    StepSequencer s; auto c = baseCfg(); c.gate = 0.5f;
    c.cells[0][0] = StepSequencer::On;
    s.setConfig (c);
    auto e = run (s, 200, 200);
    int onT = -1, offT = -1;
    for (auto& x : e) { if (x.on && onT < 0) onT = x.t; else if (! x.on && onT >= 0 && offT < 0) offT = x.t; }
    REQUIRE (offT > onT);
    REQUIRE (offT <= 60);                           // gate 0.5 * 100 -> ~50, before the next step at 100
}
