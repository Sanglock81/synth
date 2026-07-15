// ============================================================================
// Per-part MIDI looper: a recorded note is NOT double-triggered on the recording
// pass, plays back on the next loop cycle, respects PLAY, and CLEAR wipes it.
// JUCE-free (DSP-only).
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "Looper.h"
#include <vector>
#include <utility>

namespace
{
    // Run one loop cycle in `block`-sized chunks; return the (part,note) of every
    // note-ON emitted for playback during that cycle.
    std::vector<std::pair<int,int>> cycle (Looper& lp, int block)
    {
        std::vector<std::pair<int,int>> ons;
        const int len = lp.loopLength();
        for (int done = 0; done < len; done += block)
        {
            const int n = std::min (block, len - done);
            lp.playBlock (n, [&] (int part, int note, float, bool on) { if (on) ons.emplace_back (part, note); });
            lp.advance (n);
        }
        return ons;
    }
}

TEST_CASE ("looper: recorded note plays next cycle, not the recording pass", "[dsp][looper]")
{
    Looper lp; lp.setLoopLength (1000); lp.setPlaying (0, true); lp.setRecording (0, true);
    lp.recordNote (0, 100, 60, 0.8f, true);      // part 0 (lane 0), at t=100

    REQUIRE (cycle (lp, 100).empty());           // recording pass: the live note already sounded
    auto next = cycle (lp, 100);                 // next cycle: it plays back
    REQUIRE (next.size() == 1);
    REQUIRE (next[0].first == 0);
    REQUIRE (next[0].second == 60);
    // and it keeps looping
    REQUIRE (cycle (lp, 100).size() == 1);
}

TEST_CASE ("looper: per-part lanes are independent", "[dsp][looper]")
{
    Looper lp; lp.setLoopLength (800);
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
    Looper lp; lp.setLoopLength (500); lp.setRecording (0, true); lp.setPlaying (0, true);
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
