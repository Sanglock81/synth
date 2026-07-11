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

TEST_CASE ("master EQ is functional: enabling a low-shelf boost lifts a low note", "[plugin][eq]")
{
    // Render a low tone through the processor with the EQ off, then with a big low-shelf
    // boost enabled; the boosted render must carry more low-end energy. Proves the EQ is
    // wired end-to-end (param -> processBlock -> audio), not just present in the UI.
    auto renderRms = [] (bool eqOn)
    {
        VASynthProcessor p;
        p.prepareToPlay (48000.0, 128);
        p.apvts.getParameter (ParamID::eqOn)->setValueNotifyingHost (eqOn ? 1.0f : 0.0f);
        if (eqOn) p.apvts.getParameter (ParamID::eqLsGain)->setValueNotifyingHost (1.0f);  // +18 dB

        juce::AudioBuffer<float> buf (2, 128);
        juce::MidiBuffer note; note.addEvent (juce::MidiMessage::noteOn (1, 45, 0.9f), 0);   // ~110 Hz
        double rms = 0.0;
        for (int b = 0; b < 16; ++b)
        {
            buf.clear();
            juce::MidiBuffer m = (b == 0) ? note : juce::MidiBuffer();
            p.processBlock (buf, m);
            if (b >= 12)   // measure once the note has settled
            {
                const float* d = buf.getReadPointer (0);
                for (int i = 0; i < 128; ++i) rms += (double) d[i] * d[i];
            }
        }
        return std::sqrt (rms / (4 * 128));
    };

    const double off = renderRms (false);
    const double on  = renderRms (true);
    REQUIRE (off > 0.0);
    REQUIRE (on > off * 1.1);      // the low-shelf boost audibly raised the low tone
}
