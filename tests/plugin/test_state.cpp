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
    {
        // AudioParameterBool::getValue() reports the raw set value un-snapped,
        // but state persists the boolean (0/1). Compare against the snapped value
        // so the round-trip assertion is meaningful for the kill-switch toggles.
        float v = p->getValue();
        if (dynamic_cast<juce::AudioParameterBool*> (p) != nullptr) v = v >= 0.5f ? 1.0f : 0.0f;
        expected.push_back (v);
    }

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

TEST_CASE ("sequencer grid + per-step velocities round-trip through state (#54)", "[plugin][state][seq]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    VASynthProcessor src;
    src.setSeqCell (0, 0, 1);  src.setSeqStepVel (0, 0, 100);   // full
    src.setSeqCell (0, 4, 1);  src.setSeqStepVel (0, 4, 30);    // quiet grace note
    src.setSeqCell (2, 8, 1);  src.setSeqStepVel (2, 8, 175);   // accent (>100%)
    src.setSeqNote (2, 40);

    juce::MemoryBlock blob;
    src.getStateInformation (blob);

    VASynthProcessor dst;
    dst.setStateInformation (blob.getData(), (int) blob.getSize());

    REQUIRE (dst.getSeqCell (0, 0) != 0);
    REQUIRE (dst.getSeqStepVel (0, 0) == 100);
    REQUIRE (dst.getSeqStepVel (0, 4) == 30);
    REQUIRE (dst.getSeqStepVel (2, 8) == 175);
    REQUIRE (dst.getSeqCell (2, 8) == 2);          // >100% still reads back as legacy "accent"
    REQUIRE (dst.getSeqNote (2) == 40);
    REQUIRE (dst.getSeqCell (0, 1) == 0);          // untouched step stays off
}

TEST_CASE ("legacy accent cells (no seq_vel) migrate to high velocity (#54)", "[plugin][state][seq]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // Hand-build an OLD-format state directly on the APVTS tree: seq_cells is a contiguous
    // digit string (one char/step) with a legacy accent '2' at row 0 step 0, and NO seq_vel
    // property at all — so applySeqProperty must take the migrate-from-accent path.
    VASynthProcessor src;
    juce::String cells; for (int i = 0; i < VASynthProcessor::kSeqRows * VASynthProcessor::kSeqSteps; ++i) cells << (i == 0 ? '2' : '0');
    src.apvts.state.setProperty ("seq_cells", cells, nullptr);
    src.apvts.state.removeProperty ("seq_vel", nullptr);

    juce::MemoryBlock blob;
    src.getStateInformation (blob);

    VASynthProcessor dst;
    dst.setStateInformation (blob.getData(), (int) blob.getSize());

    REQUIRE (dst.getSeqCell (0, 0) != 0);              // the accented step is ON
    REQUIRE (dst.getSeqStepVel (0, 0) > 100);          // migrated to a high (accent) velocity
}
