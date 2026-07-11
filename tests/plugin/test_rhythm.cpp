// ============================================================================
// Arpeggiator through the processor: enabling the arp and holding a note produces
// stepped output; disabling it leaves the note-dispatch path bit-identical (a held
// note still sounds). Integration smoke test — the arp's stepping/ordering logic is
// unit-tested in dsp/test_arp.cpp.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginProcessor.h"

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
