// ============================================================================
// Arpeggiator: clock stepping, note ordering (modes + octaves), gate length,
// rest steps, and latch. JUCE-free (DSP-only).
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "Arpeggiator.h"
#include <vector>

namespace
{
    struct Ev { int t; int note; float vel; bool on; };

    std::vector<Ev> run (Arpeggiator& a, int totalSamples, int block)
    {
        std::vector<Ev> evs; int base = 0;
        for (int done = 0; done < totalSamples; done += block)
        {
            const int n = std::min (block, totalSamples - done);
            a.process (n, [&] (int off, int note, float vel, bool on) { evs.push_back ({ base + off, note, vel, on }); });
            base += n;
        }
        return evs;
    }

    Arpeggiator::Config baseCfg (int mode = Arpeggiator::Up)
    {
        Arpeggiator::Config c;
        c.enabled = true; c.mode = mode; c.octaves = 1; c.gate = 0.5f; c.swing = 0.0f;
        c.samplesPerStep = 100;
        c.steps.fill (1.0f);
        return c;
    }

    std::vector<int> onNotes (const std::vector<Ev>& e)
    {
        std::vector<int> v; for (auto& x : e) if (x.on) v.push_back (x.note); return v;
    }
}

TEST_CASE ("arp disabled emits nothing", "[dsp][arp]")
{
    Arpeggiator a; Arpeggiator::Config c = baseCfg(); c.enabled = false; a.setConfig (c);
    a.noteOn (60, 0.8f);
    REQUIRE (run (a, 1000, 32).empty());
}

TEST_CASE ("arp steps a single held note on the clock", "[dsp][arp]")
{
    Arpeggiator a; a.setConfig (baseCfg());
    a.noteOn (60, 0.8f);
    auto e = run (a, 1000, 32);              // 10 steps of 100 samples
    auto ons = onNotes (e);
    REQUIRE (ons.size() >= 9);
    REQUIRE (ons.size() <= 11);
    for (int n : ons) REQUIRE (n == 60);
    // every note-on is paired with a note-off (gate)
    int on = 0, off = 0; for (auto& x : e) (x.on ? on : off)++;
    REQUIRE (off >= on - 1);
}

TEST_CASE ("arp Up alternates two held notes low..high", "[dsp][arp]")
{
    Arpeggiator a; a.setConfig (baseCfg (Arpeggiator::Up));
    a.noteOn (64, 0.8f); a.noteOn (60, 0.8f);   // added high then low; Up sorts ascending
    auto ons = onNotes (run (a, 800, 40));
    REQUIRE (ons.size() >= 6);
    REQUIRE (ons[0] == 60); REQUIRE (ons[1] == 64); REQUIRE (ons[2] == 60); REQUIRE (ons[3] == 64);
}

TEST_CASE ("arp Down goes high..low", "[dsp][arp]")
{
    Arpeggiator a; a.setConfig (baseCfg (Arpeggiator::Down));
    a.noteOn (60, 0.8f); a.noteOn (67, 0.8f);
    auto ons = onNotes (run (a, 600, 40));
    REQUIRE (ons[0] == 67); REQUIRE (ons[1] == 60); REQUIRE (ons[2] == 67);
}

TEST_CASE ("arp octaves span up an octave", "[dsp][arp]")
{
    Arpeggiator a; auto c = baseCfg (Arpeggiator::Up); c.octaves = 2; a.setConfig (c);
    a.noteOn (60, 0.8f);
    auto ons = onNotes (run (a, 500, 50));
    REQUIRE (ons[0] == 60); REQUIRE (ons[1] == 72); REQUIRE (ons[2] == 60);
}

TEST_CASE ("arp gate releases the note before the next step", "[dsp][arp]")
{
    Arpeggiator a; auto c = baseCfg(); c.gate = 0.5f; a.setConfig (c);
    a.noteOn (60, 0.8f);
    auto e = run (a, 300, 300);
    // first on at ~0, its off at ~50 (gate 0.5 * 100), before the next step at 100.
    int firstOnT = -1, firstOffT = -1;
    for (auto& x : e) { if (x.on && firstOnT < 0) firstOnT = x.t; else if (! x.on && firstOnT >= 0 && firstOffT < 0) firstOffT = x.t; }
    REQUIRE (firstOffT > firstOnT);
    REQUIRE (firstOffT <= 60);
}

TEST_CASE ("arp rest step (velocity 0) plays nothing", "[dsp][arp]")
{
    Arpeggiator a; auto c = baseCfg(); c.steps.fill (0.0f); c.steps[2] = 1.0f; a.setConfig (c);
    a.noteOn (60, 0.8f);
    auto ons = onNotes (run (a, 1600, 64));   // only step index 2 of each 16 plays
    REQUIRE (! ons.empty());
    for (int n : ons) REQUIRE (n == 60);
    REQUIRE (ons.size() <= 2);                // ~one hit per 16-step bar over 16 steps
}

TEST_CASE ("arp latch keeps the chord after release", "[dsp][arp]")
{
    Arpeggiator a; auto c = baseCfg(); c.latch = true; a.setConfig (c);
    a.noteOn (60, 0.8f);
    run (a, 200, 50);
    a.noteOff (60);                           // released, but latched
    auto ons = onNotes (run (a, 400, 50));
    REQUIRE (! ons.empty());                  // still arpeggiating
    for (int n : ons) REQUIRE (n == 60);
}

TEST_CASE ("arp without latch stops when keys released", "[dsp][arp]")
{
    Arpeggiator a; a.setConfig (baseCfg());
    a.noteOn (60, 0.8f);
    run (a, 200, 50);
    a.noteOff (60);
    REQUIRE (onNotes (run (a, 400, 50)).empty());
}

TEST_CASE ("arp velocity % scales the played velocity", "[dsp][arp][vel]")
{
    auto firstVel = [] (float velScale)
    {
        Arpeggiator a; auto c = baseCfg(); c.velScale = velScale; a.setConfig (c);
        a.noteOn (60, 0.8f);                       // played velocity 0.8
        for (auto& e : run (a, 300, 50)) if (e.on) return e.vel;
        return -1.0f;
    };
    REQUIRE (firstVel (1.0f) == Catch::Approx (0.8f).margin (0.02));   // 100% -> played vel unchanged
    REQUIRE (firstVel (0.5f) == Catch::Approx (0.4f).margin (0.02));   // 50%  -> halved
}
