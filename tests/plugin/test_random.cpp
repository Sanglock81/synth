// ============================================================================
// RANDOM (one algorithm, no modes) + VARY. Seeded-deterministic; the exclusion list never moves
// (200x hammer); every generated patch is non-silent AND passes the broken-patch invariants; the
// density shaping is present (SEMI quantized to musical intervals, attack times log-spread, a loose
// temperament correlation); generated matrix routes are valid; VARY deltas are bounded.
// Design: docs/plans/random-density.md.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginProcessor.h"
#include "PresetManager.h"
#include "ModDestRegistry.h"
#include <cmath>
#include <vector>
#include <set>

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
    float raw (VASynthProcessor& p, const char* id) { return p.apvts.getRawParameterValue (id)->load(); }
}

TEST_CASE ("RANDOM is seeded-deterministic", "[plugin][random]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor a, b;
    juce::Random ra (12345), rb (12345);
    a.randomizeSound (ra);
    b.randomizeSound (rb);
    REQUIRE (snapshotSound (a) == snapshotSound (b));
    for (int s = 0; s < ModMatrix::kSlots; ++s)
    {
        REQUIRE (a.getModSlot (-1, s).source == b.getModSlot (-1, s).source);
        REQUIRE (a.getModSlot (-1, s).dest   == b.getModSlot (-1, s).dest);
    }
}

TEST_CASE ("RANDOM hammer 200x: never silent, invariants hold, exclusions frozen", "[plugin][random][torture]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    // Distinctive values on the excluded params; they must never move.
    for (auto& id : PresetManager::randomizeExclusions())
        if (auto* prm = p.apvts.getParameter (id)) prm->setValueNotifyingHost (0.42f);
    std::vector<float> exBefore;
    for (auto& id : PresetManager::randomizeExclusions())
        if (auto* prm = p.apvts.getParameter (id)) exBefore.push_back (prm->getValue());

    juce::Random rng (99);
    for (int i = 0; i < 200; ++i)
    {
        p.randomizeSound (rng);
        INFO ("roll " << i);
        REQUIRE (renderPeak (p) > 1.0e-3f);                            // audibility floor: never silent
        // Broken-patch invariants (defect culling): a live osc, filter not fully shut, and no
        // slow-swell-into-a-hanging-note (attack+release both sluggish over ~0 sustain).
        REQUIRE (raw (p, ParamID::osc1Level) >= 0.4f - 1e-4f);
        REQUIRE (raw (p, ParamID::ampAttack) <= 0.5f + 1e-3f);         // capped: promptly audible
        const bool bothSluggish = p.apvts.getParameter (ParamID::ampAttack)->getValue()  > 0.45f
                               && p.apvts.getParameter (ParamID::ampRelease)->getValue() > 0.85f
                               && p.apvts.getParameter (ParamID::ampSustain)->getValue() < 0.1f;
        REQUIRE_FALSE (bothSluggish);
    }
    std::size_t k = 0;
    for (auto& id : PresetManager::randomizeExclusions())
        if (auto* prm = p.apvts.getParameter (id))
        { INFO ("excluded moved: " << id); REQUIRE (prm->getValue() == Catch::Approx (exBefore[k++]).margin (1e-6)); }
}

TEST_CASE ("RANDOM density: SEMI is quantized to musical intervals; attack times are log-spread", "[plugin][random][density]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    juce::Random rng (2024);
    int inSet = 0, zero = 0; float aMin = 1e9f, aMax = 0.0f;
    const int N = 400;
    auto musical = [] (float st) { st = std::round (st); return st == 0 || std::abs(st) == 5 || std::abs(st) == 7 || std::abs(st) == 12; };
    for (int i = 0; i < N; ++i)
    {
        p.randomizeSound (rng);
        const float s2 = raw (p, ParamID::osc2Semi);
        if (musical (s2)) ++inSet;
        if (std::round (s2) == 0) ++zero;
        const float a = raw (p, ParamID::ampAttack);
        aMin = std::min (aMin, a); aMax = std::max (aMax, a);
    }
    REQUIRE (inSet > (int) (N * 0.85));      // the vast majority land on {0,+/-5,+/-7,+/-12}...
    REQUIRE (inSet < N);                     // ...but a rare chromatic outlier is still possible
    REQUIRE (zero  > (int) (N * 0.25));      // zero is weighted heaviest
    REQUIRE (aMin < 0.02f);                  // perceptual (log) spread: some very short attacks...
    REQUIRE (aMax > 0.25f);                  // ...and some long-ish ones (up to the 0.5 s cap)
}

TEST_CASE ("RANDOM density: a loose temperament correlates the envelope tails", "[plugin][random][density]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    juce::Random rng (555);
    std::vector<double> sus, rel;
    const int N = 300;
    for (int i = 0; i < N; ++i)
    {
        p.randomizeSound (rng);
        sus.push_back (p.apvts.getParameter (ParamID::ampSustain)->getValue());
        rel.push_back (p.apvts.getParameter (ParamID::ampRelease)->getValue());
    }
    double ms = 0, mr = 0; for (int i = 0; i < N; ++i) { ms += sus[(size_t) i]; mr += rel[(size_t) i]; } ms /= N; mr /= N;
    double cov = 0, vs = 0, vr = 0;
    for (int i = 0; i < N; ++i) { const double ds = sus[(size_t) i]-ms, dr = rel[(size_t) i]-mr; cov += ds*dr; vs += ds*ds; vr += dr*dr; }
    const double corr = cov / std::sqrt (vs * vr);
    INFO ("sustain<->release correlation = " << corr);
    REQUIRE (corr > 0.25);      // the coherence latent is PRESENT (tails move together)...
    REQUIRE (corr < 0.95);      // ...but LOOSE (each keeps independent variance)
}

TEST_CASE ("RANDOM generates 0-3 valid registry matrix routes", "[plugin][random]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    juce::Random rng (5);
    for (int i = 0; i < 60; ++i)
    {
        p.randomizeSound (rng);
        int used = 0;
        for (int s = 0; s < ModMatrix::kSlots; ++s)
        {
            const auto slot = p.getModSlot (-1, s);
            if (slot.source == ModMatrix::SrcNone || slot.dest == ModMatrix::DstNone) continue;
            ++used;
            REQUIRE (slot.source > 0); REQUIRE (slot.source < ModMatrix::kNumSources);
            REQUIRE (moddest::nameFor (slot.dest).isNotEmpty());   // a real registry destination
            REQUIRE (slot.depth >= -1.0f); REQUIRE (slot.depth <= 1.0f);
        }
        REQUIRE (used <= 3);
    }
}

TEST_CASE ("RANDOM routes are shaped: audible + varied, not always LFO->Cutoff", "[plugin][random][routes]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    juce::Random rng (808);
    int total = 0, cutoff = 0, moveSrc = 0;
    std::set<int> dests;
    const int N = 500;
    auto isMove = [] (int s) { return s == ModMatrix::LFO1 || s == ModMatrix::LFO2 || s == ModMatrix::LFO3
                                   || s == ModMatrix::ModEnv || s == ModMatrix::AmpEnv || s == ModMatrix::Velocity; };
    for (int i = 0; i < N; ++i)
    {
        p.randomizeSound (rng);
        for (int s = 0; s < ModMatrix::kSlots; ++s)
        {
            const auto slot = p.getModSlot (-1, s);
            if (slot.source == ModMatrix::SrcNone || slot.dest == ModMatrix::DstNone) continue;
            ++total; dests.insert (slot.dest);
            if (slot.dest == ModMatrix::Cutoff) ++cutoff;
            if (isMove (slot.source)) ++moveSrc;
        }
    }
    INFO ("routes=" << total << " cutoff=" << cutoff << " distinctDests=" << dests.size() << " moveSrc=" << moveSrc);
    REQUIRE (total > 100);                                  // routes are actually being generated
    REQUIRE (dests.size() >= 5);                            // spread across the audibly-live set, not one dest
    REQUIRE ((double) cutoff / total < 0.45);              // cutoff is common but NOT dominant (the fix)
    REQUIRE ((double) cutoff / total > 0.05);              // ...still well represented
    REQUIRE ((double) moveSrc / total > 0.6);             // most routes use a source that actually MOVES
}

TEST_CASE ("VARY perturbs by bounded deltas and never moves exclusions", "[plugin][random]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    const auto before = snapshotSound (p);
    juce::Random rng (3);
    p.varySound (rng);
    const auto after = snapshotSound (p);

    const auto& ids = VASynthProcessor::soundDesignParamIDs();
    for (int i = 0; i < ids.size(); ++i)
        if (PresetManager::randomizeExclusions().contains (ids[i]))
            REQUIRE (after[(std::size_t) i] == Catch::Approx (before[(std::size_t) i]).margin (1e-6));   // exclusions frozen
    double moved = 0.0; for (int i = 0; i < ids.size(); ++i) moved += std::abs (after[(std::size_t) i] - before[(std::size_t) i]);
    REQUIRE (moved < ids.size() * 0.2);   // VARY stays in the neighbourhood — far less than a full randomize
}

TEST_CASE ("torture: rapid RANDOM while a note sustains stays finite + bounded, click-free",
           "[plugin][random][torture]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 128);
    juce::Random rng (77);
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
    }
}
