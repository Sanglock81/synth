// ============================================================================
// Macros (R2): Random assigns 1..4 distinct macros to distinct routable params,
// and the routing map persists across a state round-trip.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <set>
#include <memory>

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

TEST_CASE ("default macro map assigns M1..M8 to the factory targets (#55)", "[plugin][macros]")
{
    VASynthProcessor p;
    REQUIRE (p.getMacroTargetId (0) == ParamID::filterCutoff);
    REQUIRE (p.getMacroTargetId (1) == ParamID::filterReso);
    REQUIRE (p.getMacroTargetId (2) == ParamID::filterEnvAmt);
    REQUIRE (p.getMacroTargetId (3) == ParamID::ampRelease);
    REQUIRE (p.getMacroTargetId (4) == ParamID::lfoRate);
    REQUIRE (p.getMacroTargetId (5) == ParamID::lfoDepth);
    REQUIRE (p.getMacroTargetId (6) == ParamID::reverbMix);
    REQUIRE (p.getMacroTargetId (7) == VASynthProcessor::kFocusLevelTarget);
    REQUIRE (p.getMacroTargetName (7) == "Focus Level");
    REQUIRE (p.getMacroTargetName (0) == "Cutoff");    // resolves through the target param's name
}

TEST_CASE ("pre-#55 state without a macro_map loads the factory macro defaults", "[plugin][macros][state]")
{
    VASynthProcessor src;
    src.apvts.state.removeProperty ("macro_map", nullptr);   // an older session had no macro map
    juce::MemoryBlock blob; src.getStateInformation (blob);

    VASynthProcessor dst; dst.setStateInformation (blob.getData(), (int) blob.getSize());
    REQUIRE (dst.getMacroTargetId (0) == ParamID::filterCutoff);
    REQUIRE (dst.getMacroTargetId (7) == VASynthProcessor::kFocusLevelTarget);
}

TEST_CASE ("the focus-level macro target survives a state round-trip (#55)", "[plugin][macros][state]")
{
    VASynthProcessor src;
    src.setMacroTarget (3, VASynthProcessor::kFocusLevelTarget);   // put the sentinel somewhere custom
    juce::MemoryBlock blob; src.getStateInformation (blob);

    VASynthProcessor dst; dst.setStateInformation (blob.getData(), (int) blob.getSize());
    REQUIRE (dst.getMacroTargetId (3) == VASynthProcessor::kFocusLevelTarget);
}

TEST_CASE ("moving a macro drives its default target; M8 drives the focused part level (#55)", "[plugin][macros][editor]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 128);
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    ed->setSize (1760, 1000);

    // M1's default target is the filter cutoff: turning macro1 moves filter_cutoff to match.
    p.apvts.getParameter (ParamID::macro1)->setValueNotifyingHost (0.25f);
    REQUIRE (p.apvts.getParameter (ParamID::filterCutoff)->getValue() == Catch::Approx (0.25f).margin (1e-4));

    // M8 = focused part level: it follows the edit focus, not a fixed part.
    p.setEditFocus (0);
    p.apvts.getParameter (ParamID::macro8)->setValueNotifyingHost (0.8f);
    REQUIRE (p.apvts.getParameter (ParamID::part0Level)->getValue() == Catch::Approx (0.8f).margin (1e-4));

    p.setEditFocus (2);
    p.apvts.getParameter (ParamID::macro8)->setValueNotifyingHost (0.4f);
    REQUIRE (p.apvts.getParameter (ParamID::part2Level)->getValue() == Catch::Approx (0.4f).margin (1e-4));
    REQUIRE (p.apvts.getParameter (ParamID::part0Level)->getValue() == Catch::Approx (0.8f).margin (1e-4));  // P1 untouched
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
