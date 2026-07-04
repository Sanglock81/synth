// ============================================================================
// Plugin-layer: APVTS state round-trip. Set every parameter to a random value,
// save, reset a fresh processor, restore, assert all values match.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginProcessor.h"
#include <random>

TEST_CASE ("APVTS state round-trips every parameter", "[plugin][state]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    VASynthProcessor src;
    auto& params = src.getParameters();

    std::mt19937 rng (0xABCDEF);
    std::uniform_real_distribution<float> dist (0.0f, 1.0f);

    // Randomise every parameter, then read back the (possibly snapped) values.
    std::vector<float> expected;
    for (auto* p : params)
    {
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
            rp->setValueNotifyingHost (dist (rng));
    }
    for (auto* p : params)
        expected.push_back (p->getValue());

    juce::MemoryBlock blob;
    src.getStateInformation (blob);

    VASynthProcessor dst;
    dst.setStateInformation (blob.getData(), (int) blob.getSize());

    auto& dstParams = dst.getParameters();
    REQUIRE (dstParams.size() == params.size());
    for (int i = 0; i < dstParams.size(); ++i)
    {
        INFO ("param " << i << " = " << params[i]->getName (64));
        REQUIRE (dstParams[i]->getValue() == Catch::Approx (expected[(size_t) i]).margin (1e-5));
    }
}
