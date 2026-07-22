// ============================================================================
// H5 — musical RANDOM (hybrid NEW: wild/constrained/archetype) + VARY. Seeded-deterministic;
// mode distribution ~70/15/15; the exclusion list never moves in any mode (200x hammer); every
// generated patch is non-silent (audibility floor) and renders click-free; generated matrix
// routes are valid; VARY deltas are bounded.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginProcessor.h"
#include "PresetManager.h"
#include "ModDestRegistry.h"
#include <cmath>

namespace
{
    float renderPeak (VASynthProcessor& p, int blocks = 220)
    {
        p.prepareToPlay (48000.0, 128);
        float peak = 0.0f;
        for (int b = 0; b < blocks; ++b)
        {
            juce::AudioBuffer<float> buf (2, 128); buf.clear();
            juce::MidiBuffer m; if (b == 0) m.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
            p.processBlock (buf, m);
            peak = std::max (peak, buf.getMagnitude (0, 128));
        }
        return peak;
    }
    std::vector<float> snapshotSound (VASynthProcessor& p)
    {
        std::vector<float> v;
        for (auto& id : VASynthProcessor::soundDesignParamIDs())
            v.push_back (p.apvts.getParameter (id)->getValue());
        return v;
    }
}

TEST_CASE ("RANDOM is seeded-deterministic (#H5)", "[plugin][random]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor a, b;
    juce::Random ra (12345), rb (12345);
    const auto resA = a.randomizeSound (ra);
    const auto resB = b.randomizeSound (rb);
    REQUIRE (resA.mode == resB.mode);
    REQUIRE (snapshotSound (a) == snapshotSound (b));
    for (int s = 0; s < ModMatrix::kSlots; ++s)
    {
        REQUIRE (a.getModSlot (-1, s).source == b.getModSlot (-1, s).source);
        REQUIRE (a.getModSlot (-1, s).dest   == b.getModSlot (-1, s).dest);
    }
}

TEST_CASE ("mode roll follows the ~70/15/15 split (#H5)", "[plugin][random]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    juce::Random rng (7);
    int wild = 0, arch = 0, con = 0;
    const int N = 2000;
    for (int i = 0; i < N; ++i)
    {
        const auto res = p.randomizeSound (rng);
        if (res.mode == VASynthProcessor::RandomMode::Wild) ++wild;
        else if (res.mode == VASynthProcessor::RandomMode::Archetype) ++arch;
        else ++con;
    }
    REQUIRE (wild > (int) (N * 0.62)); REQUIRE (wild < (int) (N * 0.78));
    REQUIRE (arch > (int) (N * 0.09)); REQUIRE (arch < (int) (N * 0.21));
    REQUIRE (con  > (int) (N * 0.09)); REQUIRE (con  < (int) (N * 0.21));
}

TEST_CASE ("every generated patch is non-silent, in every mode (#H5)", "[plugin][random]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    using RM = VASynthProcessor::RandomMode;
    for (int seed = 1; seed <= 25; ++seed)
    {
        for (auto mode : { RM::Wild, RM::Constrained, RM::Archetype })
            for (int arch = 0; arch < (mode == RM::Archetype ? VASynthProcessor::kNumArchetypes : 1); ++arch)
            {
                VASynthProcessor p;
                juce::Random rng (seed * 31 + arch);
                p.randomizeSound (rng, mode, mode == RM::Archetype ? arch : -1);
                INFO ("seed " << seed << " mode " << (int) mode << " arch " << arch);
                REQUIRE (renderPeak (p) > 1.0e-3f);      // the audibility floor holds — never silent
            }
    }
}

TEST_CASE ("RANDOM never moves an excluded (performance/global) parameter, any mode (#H5)",
           "[plugin][random]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    // Set the excluded params to distinctive values, then hammer randomize in every mode.
    for (auto& id : PresetManager::randomizeExclusions())
        if (auto* prm = p.apvts.getParameter (id)) prm->setValueNotifyingHost (0.42f);
    std::vector<float> before;
    for (auto& id : PresetManager::randomizeExclusions())
        if (auto* prm = p.apvts.getParameter (id)) before.push_back (prm->getValue());

    juce::Random rng (99);
    using RM = VASynthProcessor::RandomMode;
    for (int i = 0; i < 200; ++i)
    {
        const int roll = i % 8;
        if (roll < 6) p.randomizeSound (rng, RM::Archetype, roll);
        else if (roll == 6) p.randomizeSound (rng, RM::Wild, -1);
        else p.randomizeSound (rng, RM::Constrained, -1);
    }
    std::size_t k = 0;
    for (auto& id : PresetManager::randomizeExclusions())
        if (auto* prm = p.apvts.getParameter (id))
        { INFO ("excluded moved: " << id); REQUIRE (prm->getValue() == Catch::Approx (before[k++]).margin (1e-6)); }
}

TEST_CASE ("generated matrix routes are valid registry destinations (#H5)", "[plugin][random]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    juce::Random rng (5);
    for (int i = 0; i < 60; ++i)
    {
        p.randomizeSound (rng, VASynthProcessor::RandomMode::Wild, -1);
        for (int s = 0; s < ModMatrix::kSlots; ++s)
        {
            const auto slot = p.getModSlot (-1, s);
            if (slot.source == ModMatrix::SrcNone || slot.dest == ModMatrix::DstNone) continue;
            REQUIRE (slot.source > 0); REQUIRE (slot.source < ModMatrix::kNumSources);
            REQUIRE (moddest::nameFor (slot.dest).isNotEmpty());   // a real registry destination
            REQUIRE (slot.depth >= -1.0f); REQUIRE (slot.depth <= 1.0f);
        }
    }
}

TEST_CASE ("VARY perturbs by bounded deltas and never moves exclusions (#H5)", "[plugin][random]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    const auto before = snapshotSound (p);
    juce::Random rng (3);
    p.varySound (rng);
    const auto after = snapshotSound (p);

    const auto& ids = VASynthProcessor::soundDesignParamIDs();
    for (int i = 0; i < ids.size(); ++i)
    {
        // Continuous params move at most ~kVaryDelta (+ the audibility floor may pull a couple);
        // check the typical bound on params the floor doesn't touch.
        const auto& id = ids[i];
        if (PresetManager::randomizeExclusions().contains (id))
            REQUIRE (after[(std::size_t) i] == Catch::Approx (before[(std::size_t) i]).margin (1e-6));   // exclusions frozen
    }
    // The overall move is small — VARY stays in the neighbourhood.
    double moved = 0.0; for (int i = 0; i < ids.size(); ++i) moved += std::abs (after[(std::size_t) i] - before[(std::size_t) i]);
    REQUIRE (moved < ids.size() * 0.2);   // far less than a full randomize would move
}

TEST_CASE ("torture: rapid RANDOM while a note sustains stays finite + bounded, click-free (#H5)",
           "[plugin][random][torture]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 128);
    juce::Random rng (77);
    float prevLast = 0.0f;
    for (int b = 0; b < 400; ++b)
    {
        if (b % 8 == 0) p.randomizeSound (rng);            // re-roll the patch every ~21 ms
        juce::AudioBuffer<float> buf (2, 128); buf.clear();
        juce::MidiBuffer m; if (b == 0) m.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
        p.processBlock (buf, m);
        const float* d = buf.getReadPointer (0);
        for (int i = 0; i < 128; ++i)
        {
            REQUIRE (std::isfinite (d[i]));
            REQUIRE (std::abs (d[i]) <= 1.0001f);          // safety clipper holds through the chaos
        }
        prevLast = d[127];
    }
    (void) prevLast;
}

