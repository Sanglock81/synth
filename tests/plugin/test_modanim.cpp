// ============================================================================
// H1 — mod-indicator MOTION model: the animation is an ECHO of a value in motion, not a
// "route exists" light. A static routed offset shows nothing at rest; a moving one stays
// lit; a twist flares and decays. Pure logic (timestamps injected), so it is deterministic.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "UI/ModAnim.h"
#include <cmath>

namespace
{
    constexpr juce::uint32 kDtMs = (juce::uint32) (1000 / modanim::kTimerHz);   // one tick

    // Advance the state `n` ticks feeding a constant sample; return final ms.
    juce::uint32 hold (modanim::State& st, float sample, int n, juce::uint32 ms)
    {
        for (int i = 0; i < n; ++i) { st.tick (sample, ms); ms += kDtMs; }
        return ms;
    }
}

TEST_CASE ("a static routed offset shows nothing at rest (#H1)", "[plugin][modanim]")
{
    modanim::State st;
    juce::uint32 ms = 1000;
    ms = hold (st, 0.5f, 40, ms);       // a fixed offset the whole time (e.g. a parked macro)
    REQUIRE_FALSE (st.visible());        // never lit — no motion ever occurred
}

TEST_CASE ("a continuously-moving offset (LFO) stays lit (#H1)", "[plugin][modanim]")
{
    modanim::State st;
    juce::uint32 ms = 1000;
    st.tick (0.0f, ms); ms += kDtMs;                       // prime
    bool everDark = false;
    for (int i = 0; i < 120; ++i)                          // ~4 s of an LFO sweeping the offset
    {
        const float v = 0.5f * std::sin (i * 0.4f);        // well above kMotionEps per tick
        st.tick (v, ms); ms += kDtMs;
        if (i > 4 && ! st.visible()) everDark = true;
    }
    REQUIRE_FALSE (everDark);             // motion keeps it continuously alive
    REQUIRE (st.visible());
}

TEST_CASE ("a twist flares then decays to nothing (#H1)", "[plugin][modanim]")
{
    modanim::State st;
    juce::uint32 ms = 1000;
    st.tick (0.0f, ms); ms += kDtMs;                       // prime at rest (invisible)
    REQUIRE_FALSE (st.visible());

    st.tick (0.6f, ms); ms += kDtMs;                       // the twist: a jump -> flares
    REQUIRE (st.visible());
    REQUIRE (st.alpha() > 0.9f);

    ms = hold (st, 0.6f, 45, ms);                          // now parked at the new value ~1.5 s
    REQUIRE_FALSE (st.visible());                          // ...and it has decayed away

    st.tick (0.2f, ms);                                    // motion resumes -> re-lights immediately
    REQUIRE (st.visible());
}
