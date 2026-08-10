// ============================================================================
// AudioLoop (Group 3): stereo tape loop. Preallocated ring; overdub-by-sum record,
// add-on-play, wrap-safe indexing, length clamp. JUCE-free (Catch2 only).
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "AudioLoop.h"
#include <vector>
#include <cmath>

TEST_CASE ("AudioLoop snapshot + reload round-trips the recorded region (J3 scenes)", "[audioloop][scene]")
{
    AudioLoop lp; lp.prepare (1000); lp.setLoopLength (200); lp.setRecording (true);
    std::vector<float> inL (128, 0.5f), inR (128, -0.3f);
    lp.recordBlock (inL.data(), inR.data(), 128);
    REQUIRE (lp.hasContent());

    std::vector<float> sL, sR; lp.snapshotInto (sL, sR);
    REQUIRE ((int) sL.size() == lp.contentLength());     // exactly the recorded region (loopLen = 200)
    REQUIRE (sL[0] == Catch::Approx (0.5f));
    REQUIRE (sR[0] == Catch::Approx (-0.3f));

    lp.clear(); REQUIRE_FALSE (lp.hasContent());          // wipe...
    lp.loadFrom (sL, sR);                                 // ...and recall from the snapshot
    REQUIRE (lp.hasContent());
    std::vector<float> outL (128, 0.0f), outR (128, 0.0f);
    lp.setPlaying (true); lp.playBlock (outL.data(), outR.data(), 128);
    REQUIRE (outL[0] == Catch::Approx (0.5f));
    REQUIRE (outR[0] == Catch::Approx (-0.3f));

    // An empty recall clears the lane.
    lp.loadFrom ({}, {});
    REQUIRE_FALSE (lp.hasContent());
}

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
    // Check the content OUTSIDE the seam crossfade (the last loopLen/4 samples ramp toward buf[0]
    // to declick the wrap, #146 — that region is covered by the [audioloop][click] tests).
    const int seam = 100 / 4;
    for (int i = 0; i < 100 - seam; ++i)
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

// #146 Bug A (audio lane): the loop seam is declicked. Record a ramp whose end (~0.8) and start (0)
// don't match — the raw wrap is a ~0.8 step (a click). The tail-ramp-to-head[0] crossfade must keep
// every sample-to-sample jump small across two full loops (i.e. across the seam).
TEST_CASE ("AudioLoop: the loop seam is declicked at the wrap (#146)", "[audioloop][click]")
{
    const int len = 480;                              // > 4*seam so the crossfade window fits
    AudioLoop lp; lp.prepare (len); lp.setLoopLength (len);
    std::vector<float> in ((std::size_t) len);
    for (int i = 0; i < len; ++i) in[(std::size_t) i] = 0.8f * (float) i / (float) len;   // 0 -> ~0.8 ramp
    lp.setRecording (true); lp.recordBlock (in.data(), in.data(), len); lp.setRecording (false);
    lp.setPlaying (true);

    std::vector<float> o; o.reserve ((std::size_t) (2 * len));
    for (int done = 0; done < 2 * len; )
    {
        const int nb = std::min (64, 2 * len - done);
        std::vector<float> bL ((std::size_t) nb, 0.0f), bR ((std::size_t) nb, 0.0f);
        lp.playBlock (bL.data(), bR.data(), nb); lp.advance (nb);
        for (int i = 0; i < nb; ++i) o.push_back (bL[(std::size_t) i]);
        done += nb;
    }
    float maxJump = 0.0f;
    for (std::size_t i = 1; i < o.size(); ++i) maxJump = std::max (maxJump, std::abs (o[i] - o[i - 1]));
    INFO ("maxJump=" << maxJump);
    REQUIRE (maxJump < 0.05f);                        // raw seam step is ~0.8; the crossfade keeps it tiny
}

// The declick must be TRANSPARENT on steady/sustained material — no dip at the seam (the "inaudible
// on a pad" guarantee): a DC loop plays back at its level everywhere, including across the wrap.
TEST_CASE ("AudioLoop: seam declick is transparent on sustained content (#146)", "[audioloop][click]")
{
    const int len = 400;
    AudioLoop lp; lp.prepare (len); lp.setLoopLength (len);
    std::vector<float> in ((std::size_t) len, 0.5f);                       // steady DC (start == end)
    lp.setRecording (true); lp.recordBlock (in.data(), in.data(), len); lp.setRecording (false);
    lp.setPlaying (true);
    lp.advance (len - 32);                                                 // position so a block spans the seam
    std::vector<float> oL (64, 0.0f), oR (64, 0.0f);
    lp.playBlock (oL.data(), oR.data(), 64);                              // crosses buf[len-1] -> buf[0]
    for (int i = 0; i < 64; ++i) REQUIRE (oL[i] == Catch::Approx (0.5f).margin (1e-4));   // no dip, no step
}
