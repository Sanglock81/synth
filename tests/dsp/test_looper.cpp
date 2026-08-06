// ============================================================================
// Per-part MIDI looper: a recorded note is NOT double-triggered on the recording
// pass, plays back on the next loop cycle, respects PLAY, and CLEAR wipes it.
// J2: each lane has its OWN length; a single master clock drives per-lane phase
// (masterPos % laneLen), so different-length lanes stay aligned to one downbeat.
// JUCE-free (DSP-only).
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "Looper.h"
#include <vector>
#include <utility>
#include <algorithm>

namespace
{
    // Single-lane test helper: lane length == master wrap.
    void setLen (Looper& lp, int part, int samples)
    {
        lp.setMasterLength (samples);
        lp.setLoopLength (part, samples);
    }

    // Run one cycle of lane 0 in `block`-sized chunks; return every playback note-ON emitted.
    std::vector<std::pair<int,int>> cycle (Looper& lp, int block, int part = 0)
    {
        std::vector<std::pair<int,int>> ons;
        const int len = lp.loopLength (part);
        for (int done = 0; done < len; done += block)
        {
            const int n = std::min (block, len - done);
            lp.playBlock (n, [&] (int p, int note, float, bool on) { if (on) ons.emplace_back (p, note); });
            lp.advance (n);
        }
        return ons;
    }
}

TEST_CASE ("looper: recorded note plays next cycle, not the recording pass", "[dsp][looper]")
{
    Looper lp; setLen (lp, 0, 1000); lp.setPlaying (0, true); lp.setRecording (0, true);
    lp.recordNote (0, 100, 60, 0.8f, true);      // part 0 (lane 0), at t=100

    REQUIRE (cycle (lp, 100).empty());           // recording pass: the live note already sounded
    auto next = cycle (lp, 100);                 // next cycle: it plays back
    REQUIRE (next.size() == 1);
    REQUIRE (next[0].first == 0);
    REQUIRE (next[0].second == 60);
    // and it keeps looping
    REQUIRE (cycle (lp, 100).size() == 1);
}

// P0 (post-1.0 hands-on): after a couple loops the MIDI looper stacks OVERLAPPING triggers.
// A recorded note with a duration (on + off) must, every steady-state cycle, emit EXACTLY one
// on and one off (no doubling) and never leave the note held across the loop top (no hanging
// voice that the next cycle's on stacks on). Track the running held-count over many cycles.
TEST_CASE ("looper: multi-cycle playback does not accumulate or double-trigger (P0)", "[dsp][looper]")
{
    Looper lp; const int len = 1000; setLen (lp, 0, len);
    lp.setQuantize (0, false);
    lp.setPlaying (0, true); lp.setRecording (0, true);
    lp.recordNote (0, 100, 60, 0.8f, true);      // a normal recorded note: on @100 ...
    lp.recordNote (0, 600, 60, 0.0f, false);     // ... off @600
    lp.setRecording (0, false);

    int held = 0, maxHeld = 0, minHeld = 0;
    for (int c = 0; c < 6; ++c)
    {
        int ons = 0, offs = 0;
        for (int done = 0; done < len; done += 128)
        {
            const int n = std::min (128, len - done);
            lp.playBlock (n, [&] (int, int note, float, bool on)
            {
                if (note != 60) return;
                if (on) { ++ons; ++held; } else { ++offs; --held; }
                maxHeld = std::max (maxHeld, held); minHeld = std::min (minHeld, held);
            });
            lp.advance (n);
        }
        INFO ("cycle " << c << " ons=" << ons << " offs=" << offs << " held=" << held);
        if (c >= 1)                              // cycle 0 arms; steady state from cycle 1
        {
            REQUIRE (ons  == 1);                 // exactly one trigger per cycle (no doubling)
            REQUIRE (offs == 1);                 // exactly one release per cycle (no dropped/hanging off)
        }
    }
    REQUIRE (maxHeld <= 1);                       // the note is never stacked on itself
    REQUIRE (minHeld >= 0);                       // no spurious extra note-off
    REQUIRE (held == 0);                          // balanced at the end
}

TEST_CASE ("looper: per-part lanes are independent", "[dsp][looper]")
{
    Looper lp; lp.setMasterLength (800); lp.setLoopLength (0, 800); lp.setLoopLength (2, 800);
    for (int pt : { 0, 2 }) { lp.setRecording (pt, true); lp.setPlaying (pt, true); }
    lp.recordNote (0, 50,  60, 0.8f, true);
    lp.recordNote (2, 200, 67, 0.8f, true);
    cycle (lp, 64);                              // arm
    auto ons = cycle (lp, 64);
    REQUIRE (ons.size() == 2);
    bool p0 = false, p2 = false;
    for (auto& o : ons) { if (o.first == 0 && o.second == 60) p0 = true; if (o.first == 2 && o.second == 67) p2 = true; }
    REQUIRE (p0); REQUIRE (p2);
    REQUIRE (lp.hasContent (0)); REQUIRE (lp.hasContent (2)); REQUIRE_FALSE (lp.hasContent (1));
}

TEST_CASE ("looper: PLAY off is silent; CLEAR wipes", "[dsp][looper]")
{
    Looper lp; setLen (lp, 0, 500); lp.setRecording (0, true); lp.setPlaying (0, true);
    lp.recordNote (0, 10, 60, 0.8f, true);
    cycle (lp, 50);                              // arm

    lp.setPlaying (0, false);
    REQUIRE (cycle (lp, 50).empty());

    lp.setPlaying (0, true);
    REQUIRE (cycle (lp, 50).size() == 1);
    lp.clear (0);
    REQUIRE_FALSE (lp.hasContent (0));
    REQUIRE (cycle (lp, 50).empty());
}

TEST_CASE ("looper: 1/32 quantize snaps sloppy input to the grid, keeps note pairing", "[dsp][looper][quant]")
{
    Looper lp; setLen (lp, 0, 1600); lp.setRecording (0, true);
    lp.setQuantizeGrid (100);            // 1/32 == 100 samples
    lp.setQuantize (0, true);

    lp.recordNote (0, 137, 60, 0.8f, true);   // sloppy on: 137 -> nearest grid 100
    REQUIRE (lp.event (0, 0).t == 100);
    lp.recordNote (0, 137, 60, 0.0f, false);  // off snaps onto the on -> +1 grid (no zero-length)
    REQUIRE (lp.event (0, 1).t == 200);
    lp.recordNote (0, 260, 62, 0.8f, true);   // 260 -> nearest grid 300
    REQUIRE (lp.event (0, 2).t == 300);

    // Quantize off: the timestamp is left exactly where it was played.
    Looper raw; setLen (raw, 0, 1600); raw.setRecording (0, true);
    raw.setQuantizeGrid (100); raw.setQuantize (0, false);
    raw.recordNote (0, 137, 60, 0.8f, true);
    REQUIRE (raw.event (0, 0).t == 137);
}

// ---- J2: per-lane length ---------------------------------------------------

TEST_CASE ("looper J2: a short lane wraps N times inside a long lane, phase-aligned", "[dsp][looper][j2]")
{
    // Lane 0 = 400 (short), lane 1 = 1600 (4x). Master wraps at 1600 (a common multiple).
    Looper lp; lp.setMasterLength (1600);
    lp.setLoopLength (0, 400); lp.setLoopLength (1, 1600);
    for (int pt : { 0, 1 }) { lp.setRecording (pt, true); lp.setPlaying (pt, true); }
    lp.recordNote (0, 10, 60, 0.8f, true);       // one note near each lane's downbeat
    lp.recordNote (1, 10, 72, 0.8f, true);

    // Play one FULL master cycle (1600) in 100-sample blocks, from a fresh clock. First pass
    // arms; then run 1600 more and count.
    auto run = [&] (int total, int block) {
        int short0 = 0, long1 = 0;
        for (int done = 0; done < total; done += block)
        {
            lp.playBlock (block, [&] (int p, int, float, bool on) { if (on) { if (p == 0) ++short0; else if (p == 1) ++long1; } });
            lp.advance (block);
        }
        return std::pair<int,int> { short0, long1 };
    };
    run (1600, 100);                             // arm pass (nothing armed yet on the very first hit)
    auto [short0, long1] = run (1600, 100);      // one master cycle after arming
    REQUIRE (short0 == 4);                        // 1600 / 400 = 4 wraps -> 4 hits
    REQUIRE (long1  == 1);                        // the 1600 lane fires once
    // The short lane's downbeat coincides with the long lane's downbeat (both fired at phase ~0).
}

TEST_CASE ("looper J2: lane phase = masterPos % laneLen, continuous across the master wrap", "[dsp][looper][j2]")
{
    Looper lp; lp.setMasterLength (1200);
    lp.setLoopLength (0, 300);                    // 1200 / 300 = 4
    REQUIRE (lp.position (0) == 0);
    lp.advance (250); REQUIRE (lp.position (0) == 250);
    lp.advance (100); REQUIRE (lp.position (0) == 50);     // wrapped the 300 lane (350 % 300)
    // advance to just before the master wrap (master at 350 -> +850 = 1200 -> wraps to 0)
    lp.advance (850);
    REQUIRE (lp.position () == 0);                          // master wrapped
    REQUIRE (lp.position (0) == 0);                         // lane phase stays aligned (1200 % 300 == 0)
}

// #135: the record->play handoff must catch the just-captured downbeat on the SAME cycle (no dead
// cycle). After a lane wraps into playback, its position is a few samples past 0, so the normal
// [pos, pos+n) window would MISS a t=0 downbeat event and only play it a full loop later.
// requestDownbeatCatchUp() widens the next window to start at 0, once, so the downbeat sounds.
TEST_CASE ("looper: record->play handoff catches the downbeat (no dead cycle) (#135)", "[dsp][looper][timing]")
{
    Looper lp;
    const int len = 4800;
    lp.setMasterLength (len);
    lp.setLoopLength (0, len);
    lp.setQuantize (0, false);
    lp.setPlaying (0, true);

    // Record a downbeat event at t=0 and one mid-loop, then cross the wrap so they arm and pos > 0.
    lp.setRecording (0, true);
    lp.recordNote (0, 0, 60, 1.0f, true);          // downbeat (t == 0)
    lp.advance (2400);
    lp.recordNote (0, 0, 64, 1.0f, true);          // mid-loop (t == 2400)
    lp.advance (2390);                             // pos -> 4790
    lp.setRecording (0, false);
    lp.advance (20);                               // crosses the wrap: events arm, pos -> 10

    auto play = [&] {
        std::vector<int> fired;
        lp.playBlock (128, [&] (int, int note, float, bool on) { if (on) fired.push_back (note); });
        return fired;
    };

    // WITHOUT catch-up: window is [10, 138) -> the t=0 downbeat (60) is missed.
    {
        auto f = play();
        REQUIRE (std::find (f.begin(), f.end(), 60) == f.end());
    }
    // WITH catch-up: the same downbeat is emitted immediately (window widened to start at 0).
    lp.requestDownbeatCatchUp (0);
    {
        auto f = play();
        REQUIRE (std::find (f.begin(), f.end(), 60) != f.end());   // downbeat caught, no dead cycle
    }
    // Catch-up is one-shot: it does not re-fire the downbeat on the following block.
    {
        auto f = play();
        REQUIRE (std::find (f.begin(), f.end(), 60) == f.end());
    }
}
