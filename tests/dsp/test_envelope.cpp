// ============================================================================
// ADSR envelope tests, written to the Phase 2 spec. Two of these intentionally
// fail against the skeleton's timing calibration (release-to--80dB and
// steal()<10ms) and drive the Phase 3 re-calibration (test-first).
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "ADSREnvelope.h"
#include "test_util.h"
#include <vector>
#include <cmath>

namespace
{
    constexpr double kSR = 48000.0;

    ADSREnvelope makeEnv (double a, double d, double s, double r)
    {
        ADSREnvelope e; e.prepare (kSR); e.setParameters (a, d, s, r); return e;
    }
}

TEST_CASE ("attack reaches 1.0 within attack-time x 1.5", "[env][attack]")
{
    const double attack = 0.1;
    auto e = makeEnv (attack, 0.2, 0.8, 0.2);
    e.noteOn();

    const int limit = int (attack * 1.5 * kSR);
    bool reached = false;
    for (int i = 0; i < limit; ++i)
        if (e.nextSample() >= 0.999f) { reached = true; break; }

    REQUIRE (reached);
}

TEST_CASE ("release decays below -80 dB within release-time x 2", "[env][release]")
{
    const double release = 0.2;
    auto e = makeEnv (0.001, 0.001, 0.7, release);
    e.noteOn();
    for (int i = 0; i < int (0.05 * kSR); ++i) e.nextSample();   // settle to sustain
    e.noteOff();

    const int limit = int (release * 2.0 * kSR);
    const float floorLvl = 1.0e-4f;                              // -80 dB
    bool belowFloor = false;
    for (int i = 0; i < limit; ++i)
        if (e.nextSample() < floorLvl) { belowFloor = true; break; }

    REQUIRE (belowFloor);
}

TEST_CASE ("retrigger from mid-release has no envelope jump > 0.1", "[env][retrigger]")
{
    auto e = makeEnv (0.02, 0.05, 0.7, 0.3);
    e.noteOn();
    for (int i = 0; i < int (0.1 * kSR); ++i) e.nextSample();    // to sustain
    e.noteOff();
    for (int i = 0; i < int (0.05 * kSR); ++i) e.nextSample();   // partway through release

    const float before = e.nextSample();
    e.noteOn();                                                  // retrigger
    const float after = e.nextSample();

    INFO ("before=" << before << " after=" << after);
    REQUIRE (std::abs (after - before) <= 0.1f);
}

TEST_CASE ("steal() (quickRelease) completes within 10 ms", "[env][steal]")
{
    auto e = makeEnv (0.001, 0.001, 0.7, 0.5);
    e.noteOn();
    for (int i = 0; i < int (0.05 * kSR); ++i) e.nextSample();   // to sustain
    e.quickRelease();                                           // voice steal fade

    const int limit = int (0.010 * kSR);                        // 10 ms
    int settled = -1;
    for (int i = 0; i < limit + 1; ++i)
    {
        e.nextSample();
        if (! e.isActive()) { settled = i; break; }
    }
    INFO ("settled sample = " << settled << " (limit " << limit << ")");
    REQUIRE (settled >= 0);
}

TEST_CASE ("release segment itself is monotone & click-free", "[env][release]")
{
    auto e = makeEnv (0.001, 0.001, 0.9, 0.1);
    e.noteOn();
    for (int i = 0; i < int (0.05 * kSR); ++i) e.nextSample();
    e.noteOff();

    std::vector<float> rel;
    for (int i = 0; i < int (0.3 * kSR); ++i) rel.push_back (e.nextSample());
    REQUIRE (tu::maxDelta (rel) < 0.05f);
}
