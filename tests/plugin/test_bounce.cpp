// ============================================================================
// #98 Session export — the offline DAW-handoff bounce. A seq pattern on P1 is
// rendered offline to master.wav + a per-part stem + manifest.json; the stems sum
// to the master (clean handoff) and the render has the right length.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "PluginProcessor.h"
#include <cmath>

namespace
{
    // Load a stereo WAV into a buffer; returns sample count (0 on failure).
    int loadWav (const juce::File& f, juce::AudioBuffer<float>& out)
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatReader> r (wav.createReaderFor (f.createInputStream().release(), true));
        if (r == nullptr) return 0;
        out.setSize ((int) r->numChannels, (int) r->lengthInSamples);
        r->read (&out, 0, (int) r->lengthInSamples, 0, true, true);
        return (int) r->lengthInSamples;
    }

    void setParam (VASynthProcessor& p, const char* id, float real)
    {
        auto* pp = p.apvts.getParameter (id);
        pp->setValueNotifyingHost (pp->convertTo0to1 (real));
    }
}

TEST_CASE ("bounce: a seq session renders to master + stem + manifest, stems sum to master", "[plugin][bounce]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VASynthProcessor p;
    setParam (p, ParamID::masterGain, 1.0f);     // unity, set BEFORE prepare so the smoother primes to 1 (no ramp)
    p.prepareToPlay (48000.0, 512);

    setParam (p, ParamID::tempo, 120.0f);        // 1 bar = 96000 samples @ 48k
    setParam (p, ParamID::seqTarget, 0.0f);      // P1 (a synth part)
    p.setSeqNote (0, 60);
    for (int s = 0; s < 16; s += 4) { p.setSeqCell (0, s, 1); p.setSeqStepVel (0, s, 40); }   // four quiet hits (no clip)
    setParam (p, ParamID::seqOn, 1.0f);

    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("vasynth_bounce_test");
    dir.deleteRecursively();

    REQUIRE (p.bounceSession (dir, /*bars*/ 1));
    REQUIRE (dir.getChildFile ("master.wav").existsAsFile());
    REQUIRE (dir.getChildFile ("manifest.json").existsAsFile());
    REQUIRE (dir.getChildFile ("part1.wav").existsAsFile());          // P1 sounded -> a stem

    juce::AudioBuffer<float> master;
    const int n = loadWav (dir.getChildFile ("master.wav"), master);
    REQUIRE (n == 96000);                                             // exactly one bar
    REQUIRE (master.getMagnitude (0, n) > 0.02f);                     // the seq actually rendered

    // Sum every part stem; it must match the master (stems are the exact per-part contributions;
    // unity gain + no clip -> master == sum, within 24-bit quantisation).
    juce::AudioBuffer<float> sum (2, n); sum.clear();
    for (int part = 1; part <= 4; ++part)
    {
        auto f = dir.getChildFile ("part" + juce::String (part) + ".wav");
        if (! f.existsAsFile()) continue;
        juce::AudioBuffer<float> stem;
        if (loadWav (f, stem) == n)
            for (int ch = 0; ch < 2; ++ch) sum.addFrom (ch, 0, stem, ch, 0, n);
    }
    float maxDiff = 0.0f;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < n; ++i)
            maxDiff = std::max (maxDiff, std::abs (sum.getSample (ch, i) - master.getSample (ch, i)));
    INFO ("max |sum(stems) - master| = " << maxDiff);
    REQUIRE (maxDiff < 1.0e-3f);                                      // clean DAW handoff

    // The manifest carries the tempo the DAW lines up to.
    const auto manifest = dir.getChildFile ("manifest.json").loadFileAsString();
    REQUIRE (manifest.contains ("\"bpm\": 120"));
    REQUIRE (manifest.contains ("\"bars\": 1"));

    // The seq part's MIDI is emitted with the four hits (as note-on/off pairs).
    auto midF = dir.getChildFile ("part1.mid");
    REQUIRE (midF.existsAsFile());
    juce::MidiFile mf;
    juce::FileInputStream in (midF);
    REQUIRE (mf.readFrom (in));
    int noteOns = 0;
    for (int t = 0; t < mf.getNumTracks(); ++t)
        if (auto* trk = mf.getTrack (t))
            for (int i = 0; i < trk->getNumEvents(); ++i)
                if (trk->getEventPointer (i)->message.isNoteOn()) ++noteOns;
    REQUIRE (noteOns == 4);          // four seq hits -> four note-ons

    dir.deleteRecursively();
}

TEST_CASE ("bounce: realignBars defaults to 1 with no loops; the override wins", "[plugin][bounce]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 512);
    REQUIRE (p.realignBars() == 1);

    setParam (p, ParamID::tempo, 120.0f);
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("vasynth_bounce_test2");
    dir.deleteRecursively();
    REQUIRE (p.bounceSession (dir, /*bars*/ 2));      // override -> 2 bars
    juce::AudioBuffer<float> master;
    REQUIRE (loadWav (dir.getChildFile ("master.wav"), master) == 192000);   // two bars
    dir.deleteRecursively();
}
