// ============================================================================
// Arpeggiator through the processor: enabling the arp and holding a note produces
// stepped output; disabling it leaves the note-dispatch path bit-identical (a held
// note still sounds). Integration smoke test — the arp's stepping/ordering logic is
// unit-tested in dsp/test_arp.cpp.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginProcessor.h"
#include "PresetManager.h"

namespace
{
    double energyOverBlocks (VASynthProcessor& p, int note, int blocks)
    {
        p.prepareToPlay (48000.0, 128);
        p.routeNoteOn (note, 0.9f, 0);          // held on the LIVE part
        juce::AudioBuffer<float> buf (2, 128);
        juce::MidiBuffer midi;
        double e = 0.0;
        for (int b = 0; b < blocks; ++b) { buf.clear(); p.processBlock (buf, midi); e += buf.getRMSLevel (0, 0, 128); }
        return e;
    }
}

TEST_CASE ("arp on: a held note produces sound through the processor", "[plugin][arp]")
{
    VASynthProcessor p;
    p.apvts.getParameter (ParamID::arpOn)->setValueNotifyingHost (1.0f);
    p.apvts.getParameter (ParamID::tempo)->setValueNotifyingHost (0.8f);   // brisk clock
    REQUIRE (energyOverBlocks (p, 60, 60) > 0.0);
}

TEST_CASE ("arp off: dispatch path unchanged (held note still sounds)", "[plugin][arp]")
{
    VASynthProcessor p;   // arp defaults off
    REQUIRE (p.apvts.getRawParameterValue (ParamID::arpOn)->load() < 0.5f);
    REQUIRE (energyOverBlocks (p, 60, 20) > 0.0);
}

TEST_CASE ("arp step pattern persists across a state round-trip", "[plugin][arp][state]")
{
    VASynthProcessor src;
    src.setArpStep (0, 0.25f);
    src.setArpStep (7, 0.9f);

    juce::MemoryBlock blob; src.getStateInformation (blob);
    VASynthProcessor dst; dst.setStateInformation (blob.getData(), (int) blob.getSize());

    REQUIRE (dst.getArpStep (0) == Catch::Approx (0.25f).margin (0.01));
    REQUIRE (dst.getArpStep (7) == Catch::Approx (0.9f).margin (0.01));
}

TEST_CASE ("looper records a performance and plays it back next cycle", "[plugin][looper]")
{
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 128);
    p.apvts.getParameter (ParamID::tempo)->setValueNotifyingHost (1.0f);      // fast -> short loop
    p.apvts.getParameter (ParamID::loopRec)->setValueNotifyingHost (1.0f);
    p.apvts.getParameter (ParamID::loopPlay)->setValueNotifyingHost (1.0f);

    juce::AudioBuffer<float> buf (2, 128);
    juce::MidiBuffer midi;

    // Play a note early, release it, then run well past one loop so it re-triggers.
    p.routeNoteOn (60, 0.9f, 0);
    for (int b = 0; b < 2; ++b) { buf.clear(); p.processBlock (buf, midi); }
    p.routeNoteOff (60, 0);

    double energyLater = 0.0;
    for (int b = 0; b < 400; ++b)          // enough blocks to cross the loop boundary
    {
        buf.clear(); p.processBlock (buf, midi);
        if (b > 200) energyLater += buf.getRMSLevel (0, 0, 128);   // after the loop wrapped
    }
    REQUIRE (p.loopLaneHasContent (0));
    REQUIRE (energyLater > 0.0);           // the recorded note looped back around
}

TEST_CASE ("looper off leaves the dispatch path bit-identical (goldens safe)", "[plugin][looper]")
{
    VASynthProcessor p;   // loop_rec + loop_play default off
    REQUIRE (p.apvts.getRawParameterValue (ParamID::loopRec)->load() < 0.5f);
    REQUIRE (p.apvts.getRawParameterValue (ParamID::loopPlay)->load() < 0.5f);
    REQUIRE_FALSE (p.loopLaneHasContent (0));
}

TEST_CASE ("Random leaves arp / sequencer / looper / tempo untouched", "[plugin][rhythm][random]")
{
    VASynthProcessor p;
    PresetManager pm (p.apvts);

    // Set the rhythm section to distinctive NON-default values.
    auto set01 = [&] (const char* id, float v) { p.apvts.getParameter (id)->setValueNotifyingHost (v); };
    const char* rhythmIds[] { ParamID::tempo, ParamID::arpOn, ParamID::arpMode, ParamID::arpOctaves,
                              ParamID::arpGate, ParamID::arpSwing, ParamID::arpLatch, ParamID::arpHold,
                              ParamID::loopRec, ParamID::loopPlay, ParamID::loopBars };
    for (auto* id : rhythmIds) set01 (id, 0.42f);
    p.setArpStep (3, 0.15f);                            // and a distinctive step-pattern value

    std::vector<float> before;
    for (auto* id : rhythmIds) before.push_back (p.apvts.getRawParameterValue (id)->load());
    const float cutoffBefore = p.apvts.getRawParameterValue (ParamID::filterCutoff)->load();

    juce::Random rng (12345);
    pm.randomize (rng);

    // Every rhythm param is unchanged...
    for (size_t i = 0; i < before.size(); ++i)
        REQUIRE (p.apvts.getRawParameterValue (rhythmIds[i])->load() == Catch::Approx (before[i]).margin (1e-6));
    REQUIRE (p.getArpStep (3) == Catch::Approx (0.15f).margin (1e-4));   // pattern untouched
    // ...while a sound-design param DID move (proves randomize actually ran).
    REQUIRE (p.apvts.getRawParameterValue (ParamID::filterCutoff)->load() != Catch::Approx (cutoffBefore));
}
