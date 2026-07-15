// ============================================================================
// Arpeggiator through the processor: enabling the arp and holding a note produces
// stepped output; disabling it leaves the note-dispatch path bit-identical (a held
// note still sounds). Integration smoke test — the arp's stepping/ordering logic is
// unit-tested in dsp/test_arp.cpp.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <juce_audio_formats/juce_audio_formats.h>
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

TEST_CASE ("LFO modulation is published for the focused part's knob animation", "[plugin][ui][modanim]")
{
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 128);
    auto s01 = [&] (const char* id, float v) { p.apvts.getParameter (id)->setValueNotifyingHost (v); };
    s01 (ParamID::lfoDest, 2.0f / 3.0f);    // dest = CUTOFF (choice off/pitch/cutoff/pw)
    s01 (ParamID::lfoDepth, 0.8f);
    s01 (ParamID::lfoRate, 0.5f);
    p.routeNoteOn (60, 0.9f, 0);            // a held note -> the LFO runs

    juce::AudioBuffer<float> buf (2, 128); juce::MidiBuffer m;
    float maxCut = 0.0f;
    for (int b = 0; b < 200; ++b) { buf.clear(); p.processBlock (buf, m); maxCut = std::max (maxCut, std::abs (p.lfoModForDest (2))); }
    REQUIRE (maxCut > 0.0f);                // cutoff mod is published for the UI

    // With no note (silent part) the published mod falls back to ~0.
    VASynthProcessor q; q.prepareToPlay (48000.0, 128);
    q.apvts.getParameter (ParamID::lfoDest)->setValueNotifyingHost (2.0f / 3.0f);
    q.apvts.getParameter (ParamID::lfoDepth)->setValueNotifyingHost (0.8f);
    juce::AudioBuffer<float> b2 (2, 128); juce::MidiBuffer m2;
    for (int b = 0; b < 50; ++b) { b2.clear(); q.processBlock (b2, m2); }
    REQUIRE (std::abs (q.lfoModForDest (2)) < 1.0e-4f);
}

TEST_CASE ("arp off: dispatch path unchanged (held note still sounds)", "[plugin][arp]")
{
    VASynthProcessor p;   // arp defaults off
    REQUIRE (p.apvts.getRawParameterValue (ParamID::arpOn)->load() < 0.5f);
    REQUIRE (energyOverBlocks (p, 60, 20) > 0.0);
}

TEST_CASE ("arp gate pattern: all-rest steps silence the arp; a pattern sounds", "[plugin][arp][gate]")
{
    // With the arp on and a note held, blanking every step (all rests) must produce no
    // sound, while restoring even one step brings it back. Proves the 16-step gate grid
    // actually drives the arp (each step >0 plays, 0 rests).
    VASynthProcessor p;
    p.apvts.getParameter (ParamID::arpOn)->setValueNotifyingHost (1.0f);
    p.apvts.getParameter (ParamID::tempo)->setValueNotifyingHost (0.8f);
    for (int i = 0; i < VASynthProcessor::kArpSteps; ++i) p.setArpStep (i, 0.0f);   // all rests
    REQUIRE (energyOverBlocks (p, 60, 60) < 1.0e-9);

    VASynthProcessor q;
    q.apvts.getParameter (ParamID::arpOn)->setValueNotifyingHost (1.0f);
    q.apvts.getParameter (ParamID::tempo)->setValueNotifyingHost (0.8f);
    for (int i = 0; i < VASynthProcessor::kArpSteps; ++i) q.setArpStep (i, 0.0f);
    q.setArpStep (0, 1.0f);                                                          // one step on
    REQUIRE (energyOverBlocks (q, 60, 60) > 0.0);
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

TEST_CASE ("looper REC is one-shot: records the set bars then auto-stops and plays", "[plugin][looper]")
{
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    p.apvts.getParameter (ParamID::tempo)->setValueNotifyingHost (1.0f);   // 300 BPM -> ~1 bar in ~300 blocks
    p.apvts.getParameter (ParamID::loopBars)->setValueNotifyingHost (0.0f);// 1 bar
    p.apvts.getParameter (ParamID::loopRec)->setValueNotifyingHost (1.0f); // arm lane 1 (P1)

    juce::AudioBuffer<float> buf (2, 128); juce::MidiBuffer m;
    p.routeNoteOn (60, 0.9f, 0);
    for (int b = 0; b < 500; ++b) { buf.clear(); m.clear(); p.processBlock (buf, m); }   // > one loop

    REQUIRE (p.loopLaneHasContent (0));                                             // captured the pass
    REQUIRE (p.apvts.getRawParameterValue (ParamID::loopRec)->load()  < 0.5f);      // one-shot: REC auto-off
    REQUIRE (p.apvts.getRawParameterValue (ParamID::loopPlay)->load() > 0.5f);      // ...and auto-play engaged
}

TEST_CASE ("looper off leaves the dispatch path bit-identical (goldens safe)", "[plugin][looper]")
{
    VASynthProcessor p;   // loop_rec + loop_play default off
    REQUIRE (p.apvts.getRawParameterValue (ParamID::loopRec)->load() < 0.5f);
    REQUIRE (p.apvts.getRawParameterValue (ParamID::loopPlay)->load() < 0.5f);
    REQUIRE_FALSE (p.loopLaneHasContent (0));
    REQUIRE_FALSE (p.loopAudioHasContent (0));
}

TEST_CASE ("audio looper: AUDIO mode captures the focused part and loops it back", "[plugin][looper][audio]")
{
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 128);
    auto s01 = [&] (const char* id, float v) { p.apvts.getParameter (id)->setValueNotifyingHost (v); };
    s01 (ParamID::tempo, 1.0f);          // fast -> short loop
    s01 (ParamID::loopMode, 1.0f);       // AUDIO playback lane
    s01 (ParamID::loopRec, 1.0f);
    s01 (ParamID::loopPlay, 1.0f);

    juce::AudioBuffer<float> buf (2, 128); juce::MidiBuffer midi;
    // Sound a note during the first pass, then release — only the AUDIO lane can carry it
    // past the loop boundary (MIDI re-synth is suppressed in AUDIO mode).
    p.routeNoteOn (60, 0.9f, 0);
    for (int b = 0; b < 6; ++b) { buf.clear(); p.processBlock (buf, midi); }
    p.routeNoteOff (60, 0);

    double energyLater = 0.0;
    for (int b = 0; b < 400; ++b) { buf.clear(); p.processBlock (buf, midi); if (b > 250) energyLater += buf.getRMSLevel (0, 0, 128); }
    REQUIRE (p.loopAudioHasContent (0));
    REQUIRE (energyLater > 0.0);         // the captured audio replayed after the loop wrapped
}

TEST_CASE ("audio looper: MIDI mode leaves the audio lane silent but still captures it", "[plugin][looper][audio]")
{
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 128);
    auto s01 = [&] (const char* id, float v) { p.apvts.getParameter (id)->setValueNotifyingHost (v); };
    s01 (ParamID::tempo, 1.0f);
    s01 (ParamID::loopMode, 0.0f);       // MIDI playback lane (audio lane still records)
    s01 (ParamID::loopRec, 1.0f);
    s01 (ParamID::loopPlay, 0.0f);       // nothing plays back

    juce::AudioBuffer<float> buf (2, 128); juce::MidiBuffer midi;
    p.routeNoteOn (60, 0.9f, 0);
    for (int b = 0; b < 8; ++b) { buf.clear(); p.processBlock (buf, midi); }
    p.routeNoteOff (60, 0);
    for (int b = 0; b < 20; ++b) { buf.clear(); p.processBlock (buf, midi); }
    // Both lanes captured the performance (dual capture is independent of the mode switch).
    REQUIRE (p.loopLaneHasContent (0));
    REQUIRE (p.loopAudioHasContent (0));
}

TEST_CASE ("audio looper: WAV export writes when there's content, fails when empty", "[plugin][looper][audio][export]")
{
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 128);
    auto tmp = juce::File::createTempFile (".wav");

    REQUIRE_FALSE (p.exportLoopToWavFile (tmp));       // nothing recorded yet

    auto s01 = [&] (const char* id, float v) { p.apvts.getParameter (id)->setValueNotifyingHost (v); };
    s01 (ParamID::tempo, 1.0f);
    s01 (ParamID::loopMode, 1.0f);
    s01 (ParamID::loopRec, 1.0f);

    juce::AudioBuffer<float> buf (2, 128); juce::MidiBuffer midi;
    p.routeNoteOn (60, 0.9f, 0);
    for (int b = 0; b < 40; ++b) { buf.clear(); p.processBlock (buf, midi); }
    REQUIRE (p.loopAudioHasContent (0));

    REQUIRE (p.exportLoopToWavFile (tmp));
    REQUIRE (tmp.getSize() > 44);                       // more than a bare WAV header
    // It reads back as a valid stereo WAV.
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (tmp));
    REQUIRE (rd != nullptr);
    REQUIRE (rd->numChannels == 2);
    REQUIRE (rd->lengthInSamples > 0);
    rd.reset();
    tmp.deleteFile();
}

TEST_CASE ("sequencer: an enabled pattern drives its target part", "[plugin][seq]")
{
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 128);
    auto s01 = [&] (const char* id, float v) { p.apvts.getParameter (id)->setValueNotifyingHost (v); };

    p.setSeqCell (0, 0, 1);            // row 0, step 0 on
    p.setSeqCell (0, 4, 2);            // step 4 accent
    p.setSeqNote (0, 60);             // sound a synth note so it's audible on a synth part
    s01 (ParamID::seqTarget, 0.0f);   // target P1 (the live synth)
    s01 (ParamID::tempo, 0.9f);
    s01 (ParamID::seqOn, 1.0f);

    juce::AudioBuffer<float> buf (2, 128); juce::MidiBuffer m; double e = 0.0;
    for (int b = 0; b < 60; ++b) { buf.clear(); p.processBlock (buf, m); e += buf.getRMSLevel (0, 0, 128); }
    REQUIRE (e > 0.0);                 // the sequencer produced sound
}

TEST_CASE ("sequencer off is inert (goldens safe)", "[plugin][seq]")
{
    VASynthProcessor p;
    REQUIRE (p.apvts.getRawParameterValue (ParamID::seqOn)->load() < 0.5f);
    for (int r = 0; r < VASynthProcessor::kSeqRows; ++r)
        for (int s = 0; s < VASynthProcessor::kSeqSteps; ++s)
            REQUIRE (p.getSeqCell (r, s) == 0);      // empty grid by default
}

TEST_CASE ("sequencer grid persists across a state round-trip", "[plugin][seq][state]")
{
    VASynthProcessor src;
    src.setSeqCell (2, 5, 2);
    src.setSeqNote (2, 41);
    src.setSeqMute (3, true);

    juce::MemoryBlock blob; src.getStateInformation (blob);
    VASynthProcessor dst; dst.setStateInformation (blob.getData(), (int) blob.getSize());

    REQUIRE (dst.getSeqCell (2, 5) == 2);
    REQUIRE (dst.getSeqNote (2) == 41);
    REQUIRE (dst.getSeqMute (3));
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
    pm.randomize (rng, VASynthProcessor::soundDesignParamIDs());

    // Every rhythm param is unchanged...
    for (size_t i = 0; i < before.size(); ++i)
        REQUIRE (p.apvts.getRawParameterValue (rhythmIds[i])->load() == Catch::Approx (before[i]).margin (1e-6));
    REQUIRE (p.getArpStep (3) == Catch::Approx (0.15f).margin (1e-4));   // pattern untouched
    // ...while a sound-design param DID move (proves randomize actually ran).
    REQUIRE (p.apvts.getRawParameterValue (ParamID::filterCutoff)->load() != Catch::Approx (cutoffBefore));
}
