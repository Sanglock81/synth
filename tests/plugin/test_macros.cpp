// ============================================================================
// Macros (R2): Random assigns 1..4 distinct macros to distinct routable params,
// and the routing map persists across a state round-trip.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "PluginProcessor.h"
#include <set>

TEST_CASE ("randomizeMacros assigns 1..4 distinct routable targets", "[plugin][macros]")
{
    VASynthProcessor p;
    const auto routable = VASynthProcessor::macroRoutableIDs();

    for (int seed = 1; seed <= 40; ++seed)
    {
        juce::Random rng (seed);
        p.randomizeMacros (rng);

        int assigned = 0;
        std::set<juce::String> targets;
        for (int i = 0; i < 8; ++i)
        {
            const auto id = p.getMacroTargetId (i);
            if (id.isEmpty()) continue;
            ++assigned;
            REQUIRE (targets.insert (id).second);          // distinct target per macro
            REQUIRE (routable.contains (id));              // only curated params
        }
        REQUIRE (assigned >= 1);
        REQUIRE (assigned <= 4);
    }
}

TEST_CASE ("macro routing map persists across a state round-trip", "[plugin][macros][state]")
{
    VASynthProcessor src;
    juce::Random rng (7);
    src.randomizeMacros (rng);

    juce::MemoryBlock blob;
    src.getStateInformation (blob);

    VASynthProcessor dst;
    dst.setStateInformation (blob.getData(), (int) blob.getSize());

    for (int i = 0; i < 8; ++i)
        REQUIRE (dst.getMacroTargetId (i) == src.getMacroTargetId (i));
}

TEST_CASE ("EQ defaults keep the master output a true bypass", "[plugin][eq][state]")
{
    VASynthProcessor p;
    // eq_on defaults false; a fresh processor must report it off so the chain is skipped.
    REQUIRE (p.apvts.getRawParameterValue (ParamID::eqOn)->load() < 0.5f);
    // all band gains default to 0 dB
    for (auto* id : { ParamID::eqLsGain, ParamID::eqLmGain, ParamID::eqHmGain, ParamID::eqHsGain })
        REQUIRE (p.apvts.getRawParameterValue (id)->load() == 0.0f);
}
