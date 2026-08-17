// Synth — Copyright © 2026 John L Farmer. Licensed under AGPLv3; see LICENSE.
// ============================================================================
// Multi-row step sequencer: rows fire their notes on their on-steps, mute silences
// a row, accent raises velocity, gate releases before the next step, and independent
// rows layer. JUCE-free (DSP-only).
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "StepSequencer.h"
#include <vector>
#include <cmath>

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

TEST_CASE ("stepseq default rows are the foundational eight", "[dsp][stepseq]")
{
    // The grid ships with the eight rows a drummer reaches for first, on THIS PROJECT'S kit
    // trigger notes (not GM — every factory kit puts the rim on 39 and the open hat on 43,
    // where GM would say clap and ride). Pinned here because three copies of this array had
    // already drifted apart once, and the chromatic fallback was silently winning at startup.
    const auto d = StepSequencer::defaultNotes();
    REQUIRE (d[0] == 36);   // kick
    REQUIRE (d[1] == 38);   // snare
    REQUIRE (d[2] == 39);   // rim
    REQUIRE (d[3] == 42);   // closed hat
    REQUIRE (d[4] == 43);   // open hat
    REQUIRE (d[5] == 48);   // crash
    REQUIRE (d[6] == 47);   // ride
    REQUIRE (d[7] == 44);   // low tom
    REQUIRE (StepSequencer::Config{}.note == d);   // the DSP default really uses it
}

TEST_CASE ("stepseq layers independent rows", "[dsp][stepseq]")
{
    StepSequencer s; auto c = baseCfg();
    c.cells[0][0] = StepSequencer::On;             // kick (row 0 = note 36) on step 0
    c.cells[2][0] = StepSequencer::On;             // rim  (row 2 = note 39) on step 0 too
    s.setConfig (c);
    auto e = run (s, 200, 200);
    bool kick = false, rim = false;
    for (auto& x : e) if (x.on) { if (x.note == 36) kick = true; if (x.note == 39) rim = true; }
    REQUIRE (kick); REQUIRE (rim);
}

TEST_CASE ("stepseq mute silences a row", "[dsp][stepseq]")
{
    StepSequencer s; auto c = baseCfg();
    c.cells[0][0] = StepSequencer::On; c.mute[0] = true;
    c.cells[1][0] = StepSequencer::On;             // row 1 (note 38) still plays
    s.setConfig (c);
    auto e = run (s, 200, 200);
    for (auto& x : e) if (x.on) REQUIRE (x.note == 38);
}

TEST_CASE ("stepseq accent raises velocity", "[dsp][stepseq]")
{
    StepSequencer s; auto c = baseCfg();
    c.cells[0][0] = StepSequencer::On; c.vel[0][0] = 80;    // normal
    c.cells[0][4] = StepSequencer::On; c.vel[0][4] = 150;   // accent = higher per-step velocity (#54)
    s.setConfig (c);
    auto e = run (s, 600, 300);
    float normal = 0, accent = 0;
    for (auto& x : e) if (x.on) { if (x.t < 50) normal = x.vel; else accent = std::max (accent, x.vel); }
    REQUIRE (accent > normal);
}

TEST_CASE ("stepseq per-step velocity %: 100 = full, 30 = quiet grace note", "[dsp][stepseq][vel]")
{
    StepSequencer s; auto c = baseCfg();
    c.cells[0][0] = StepSequencer::On; c.vel[0][0] = 100;   // full
    c.cells[0][4] = StepSequencer::On; c.vel[0][4] = 30;    // grace note
    s.setConfig (c);
    auto e = run (s, 600, 300);
    float full = 0, grace = -1;
    for (auto& x : e) if (x.on) { if (x.t < 50) full = x.vel; else if (grace < 0) grace = x.vel; }
    REQUIRE (full  == Catch::Approx (1.0f).margin (0.02));   // 100% -> 1.0
    REQUIRE (grace == Catch::Approx (0.3f).margin (0.02));   // 30%  -> 0.3
    REQUIRE (grace < full * 0.5f);                           // audibly quieter
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

namespace {
    // Drive the seq exactly like PluginProcessor: phase-only realignPhase(step, into) at each bar
    // boundary, process() per block. Count kick note-ons within the N-bar window. Pre-fix (force-fire
    // realign) this doubled the downbeat with a block-size-dependent period (Bug B, #145).
    int kicksPerRunPhaseRealign (double samplesPerStep, int block, int bars)
    {
        StepSequencer s; auto c = baseCfg();
        c.samplesPerStep = samplesPerStep;
        for (int st = 0; st < 16; ++st) c.cells[0][(std::size_t) st] = (st % 4 == 0) ? StepSequencer::On : StepSequencer::Off;
        c.note[0] = 36;
        s.setConfig (c);
        const double barLen = 16.0 * samplesPerStep;
        long long masterPos = 0; int prevBar = -1, kicks = 0;
        const long long endPos = (long long) (barLen * bars);
        while (masterPos < endPos)
        {
            const int bar = (int) ((double) masterPos / barLen);
            if (bar != prevBar)
            {
                const double gp = (double) masterPos / samplesPerStep;
                const long long fg = (long long) std::floor (gp);
                s.realignPhase ((int) (((fg % 16) + 16) % 16), (gp - (double) fg) * samplesPerStep);
                prevBar = bar;
            }
            const int n = (int) std::min ((long long) block, endPos - masterPos);
            const long long base = masterPos;
            s.process (n, [&] (int off, int note, float, bool on)
                       { if (on && note == 36 && base + off < endPos - 2) ++kicks; });   // -2 drops the bar-N
                       // downbeat, which the eps clock tolerance fires ~1 sample early (it belongs to bar N)
            masterPos += n;
        }
        return kicks;
    }
}
TEST_CASE ("Bug B fixed: phase-only realign fires each downbeat exactly once, any block size (#145)", "[dsp][stepseq]")
{
    const int bars = 12;
    for (double sps : { 5512.5, 6000.0, 4801.7 })          // 44.1k/48k/odd tempo
        for (int block : { 64, 128, 256, 441, 512 })
        {
            const int kicks = kicksPerRunPhaseRealign (sps, block, bars);
            INFO ("sps=" << sps << " block=" << block);
            REQUIRE (kicks == 4 * bars);                    // 4 on the floor, no doubled downbeats
        }
}
