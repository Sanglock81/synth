// ============================================================================
// J1.1 — master tempo host-follow. In a DAW the host's BPM + play position drive the
// transport (arp/seq/looper + the synced-LFO beat clock); standalone uses the Tempo knob.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginProcessor.h"

namespace
{
    constexpr double kSR = 48000.0;

    // A minimal host playhead: reports a BPM + play state + an advancing ppq position.
    struct MockHost : juce::AudioPlayHead
    {
        double bpm = 120.0, ppq = 0.0; bool playing = true; bool provideBpm = true;
        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo pi;
            if (provideBpm) pi.setBpm (bpm);
            pi.setPpqPosition (ppq);
            pi.setIsPlaying (playing);
            return pi;
        }
    };

    void run (VASynthProcessor& p, int blocks, int n = 256)
    {
        juce::AudioBuffer<float> buf (2, n); juce::MidiBuffer m;
        for (int b = 0; b < blocks; ++b) { buf.clear(); m.clear(); p.processBlock (buf, m); }
    }
}

TEST_CASE ("effective tempo follows the host BPM when present", "[plugin][tempo][j1]")
{
    VASynthProcessor p; p.prepareToPlay (kSR, 256);
    p.apvts.getParameter (ParamID::tempo)->setValueNotifyingHost (
        p.apvts.getParameter (ParamID::tempo)->convertTo0to1 (90.0f));   // internal knob = 90

    MockHost host; host.bpm = 140.0; p.setPlayHead (&host);
    run (p, 2);
    REQUIRE (p.effectiveBpm() == Catch::Approx (140.0f).margin (0.01f));   // host wins

    host.bpm = 172.0;                                                       // host tempo change
    run (p, 2);
    REQUIRE (p.effectiveBpm() == Catch::Approx (172.0f).margin (0.01f));    // followed
}

TEST_CASE ("standalone (no host tempo) uses the internal Tempo knob", "[plugin][tempo][j1]")
{
    VASynthProcessor p; p.prepareToPlay (kSR, 256);
    p.apvts.getParameter (ParamID::tempo)->setValueNotifyingHost (
        p.apvts.getParameter (ParamID::tempo)->convertTo0to1 (96.0f));

    SECTION ("no playhead at all")
    {
        run (p, 2);
        REQUIRE (p.effectiveBpm() == Catch::Approx (96.0f).margin (0.01f));
    }
    SECTION ("a host that provides no BPM")
    {
        MockHost host; host.provideBpm = false; p.setPlayHead (&host);
        run (p, 2);
        REQUIRE (p.effectiveBpm() == Catch::Approx (96.0f).margin (0.01f));   // falls back to the knob
    }
}

// (The beat clock actually driving the DSP at the host tempo is proven end-to-end by the
//  synced-LFO rate test in test_lfo_sync — a synced 1/4 LFO measures bpm/60 Hz. effectiveBpm
//  above feeds the same samplesPerStep the arp/seq/looper read, so the tempo path is one wire.)
