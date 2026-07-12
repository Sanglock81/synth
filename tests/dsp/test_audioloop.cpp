// ============================================================================
// AudioLoop (Group 3): stereo tape loop. Preallocated ring; overdub-by-sum record,
// add-on-play, wrap-safe indexing, length clamp. JUCE-free (Catch2 only).
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "AudioLoop.h"
#include <vector>

TEST_CASE ("AudioLoop records a pass and plays it back at the same position", "[audioloop]")
{
    AudioLoop lp;
    lp.prepare (1000);
    lp.setLoopLength (100);
    REQUIRE_FALSE (lp.hasContent());

    std::vector<float> in (100, 0.5f);
    lp.setRecording (true);
    lp.recordBlock (in.data(), in.data(), 100);       // fill the whole loop
    REQUIRE (lp.hasContent());
    lp.advance (100);                                 // wrap back to 0
    REQUIRE (lp.position() == 0);

    lp.setRecording (false);
    lp.setPlaying (true);
    std::vector<float> oL (100, 0.0f), oR (100, 0.0f);
    lp.playBlock (oL.data(), oR.data(), 100);
    for (int i = 0; i < 100; ++i) { REQUIRE (oL[i] == Catch::Approx (0.5f)); REQUIRE (oR[i] == Catch::Approx (0.5f)); }
}

TEST_CASE ("AudioLoop overdub sums layers", "[audioloop]")
{
    AudioLoop lp; lp.prepare (200); lp.setLoopLength (64);
    std::vector<float> in (64, 0.3f);
    lp.setRecording (true);
    lp.recordBlock (in.data(), in.data(), 64);
    lp.recordBlock (in.data(), in.data(), 64);        // second pass at the SAME pos (no advance) -> sums
    lp.setRecording (false); lp.setPlaying (true);
    std::vector<float> oL (64, 0.0f), oR (64, 0.0f);
    lp.playBlock (oL.data(), oR.data(), 64);
    for (int i = 0; i < 64; ++i) REQUIRE (oL[i] == Catch::Approx (0.6f));   // 0.3 + 0.3
}

TEST_CASE ("AudioLoop record wraps within a block", "[audioloop]")
{
    AudioLoop lp; lp.prepare (200); lp.setLoopLength (100);
    lp.advance (80);                                  // pos = 80
    std::vector<float> in (40, 1.0f);
    lp.setRecording (true);
    lp.recordBlock (in.data(), in.data(), 40);        // writes 80..99 and 0..19
    lp.advance (40);
    REQUIRE (lp.position() == 20);                    // (80 + 40) % 100

    lp.setRecording (false); lp.setPlaying (true);
    // Read the whole loop from position 0 and confirm exactly indices [80,100) + [0,20) are set.
    lp.setLoopLength (100);
    // Rewind pos to 0 by advancing to a wrap.
    lp.advance (80);                                  // pos back to 0
    REQUIRE (lp.position() == 0);
    std::vector<float> oL (100, 0.0f), oR (100, 0.0f);
    lp.playBlock (oL.data(), oR.data(), 100);
    for (int i = 0; i < 100; ++i)
    {
        const bool written = (i >= 80) || (i < 20);
        REQUIRE (oL[i] == Catch::Approx (written ? 1.0f : 0.0f));
    }
}

TEST_CASE ("AudioLoop clear wipes content and playback is silent", "[audioloop]")
{
    AudioLoop lp; lp.prepare (128); lp.setLoopLength (128);
    std::vector<float> in (128, 0.7f);
    lp.setRecording (true); lp.recordBlock (in.data(), in.data(), 128);
    REQUIRE (lp.hasContent());
    lp.clear();
    REQUIRE_FALSE (lp.hasContent());
    REQUIRE (lp.position() == 0);
    lp.setRecording (false); lp.setPlaying (true);
    std::vector<float> oL (128, 9.0f), oR (128, 9.0f);
    lp.playBlock (oL.data(), oR.data(), 128);         // no content -> no-op (leaves the buffer as-is)
    for (int i = 0; i < 128; ++i) REQUIRE (oL[i] == Catch::Approx (9.0f));
}

TEST_CASE ("AudioLoop length never exceeds the allocated ring", "[audioloop]")
{
    AudioLoop lp; lp.prepare (50);
    lp.setLoopLength (200);
    REQUIRE (lp.loopLength() == 50);                  // clamped to capacity
    lp.setLoopLength (30);
    REQUIRE (lp.loopLength() == 30);
}

TEST_CASE ("AudioLoop is inert while not recording / not playing", "[audioloop]")
{
    AudioLoop lp; lp.prepare (128); lp.setLoopLength (128);
    std::vector<float> in (128, 1.0f);
    lp.recordBlock (in.data(), in.data(), 128);       // recording defaults OFF -> no-op
    REQUIRE_FALSE (lp.hasContent());

    lp.setRecording (true); lp.recordBlock (in.data(), in.data(), 128);
    std::vector<float> oL (128, 0.0f), oR (128, 0.0f);
    lp.playBlock (oL.data(), oR.data(), 128);         // playing defaults OFF -> no-op
    for (int i = 0; i < 128; ++i) REQUIRE (oL[i] == Catch::Approx (0.0f));
}
