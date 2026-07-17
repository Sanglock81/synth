// ============================================================================
// [6c] Processor-level plug-and-play MIDI behaviour the standalone hot-plug
// watcher drives: applying a device profile (incl. a user override), panic
// (all-notes-off) on unplug, and the toast notification channel.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginProcessor.h"
#include <cmath>

namespace
{
    constexpr double kSR = 48000.0;
    constexpr int    kBlock = 512;

    double blockRms (VASynthProcessor& p, juce::MidiBuffer midi)
    {
        juce::AudioBuffer<float> buf (2, kBlock);
        buf.clear();
        p.processBlock (buf, midi);
        double a = 0.0;
        const float* ch = buf.getReadPointer (0);
        for (int i = 0; i < kBlock; ++i) a += double (ch[i]) * ch[i];
        return std::sqrt (a / kBlock);
    }
}

TEST_CASE ("applyDeviceProfile applies a user override for a matched device", "[plugin][6c][hotplug]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // Drop a user profile on disk BEFORE constructing the processor (the library
    // loads the user dir at construction).
    auto dir = VASynthProcessor::userMidiProfileDir();
    auto file = dir.getChildFile ("zz_vasynth_unittest.json");
    // reverb_size is NOT in the built-in factory map, so it's unmapped until the
    // profile applies (reverb_mix, cutoff, etc. already have factory CCs).
    file.replaceWithText (R"({ "name":"UnitTest Pad", "match":["VASynthUnitTestPad"],
                               "pitchBendRange":2,
                               "mappings":[ {"cc":70,"param":"reverb_size"} ] })");

    VASynthProcessor p;
    auto& learn = p.getMidiLearn();
    REQUIRE (learn.getCCForParam ("reverb_size") == -1);         // nothing on CC 70 yet

    p.applyDeviceProfile ("VASynthUnitTestPad MIDI 1");
    REQUIRE (learn.getCCForParam ("reverb_size") == 70);
    REQUIRE (learn.getSource (70) == MidiLearnManager::Source::User);

    file.deleteFile();                                           // cleanup
}

TEST_CASE ("applyDeviceProfile on a factory device keeps its default map", "[plugin][6c][hotplug]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    p.applyDeviceProfile ("Novation Launchkey Mini MK3");
    REQUIRE (p.getMidiLearn().getCCForParam ("macro1") == 21);          // factory 21 -> macro1
}

TEST_CASE ("a matched device profile is authoritative: it forces its map over a stale learn", "[plugin][6c][hotplug]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;

    // A past session had CC 21 (a Launchkey pot) learned onto a synth param instead of macro1.
    p.getMidiLearn().armLearn (ParamID::filterCutoff);
    p.getMidiLearn().handleCC (1, 21, 100);
    REQUIRE (p.getMidiLearn().getCCForParam (ParamID::filterCutoff) == 21);

    // Plugging the Launchkey in (its profile applies) must RECLAIM CC 21 for macro1 — the
    // device's pots always drive the macros. Match is broad now ("Launchkey" substring).
    p.applyDeviceProfile ("Launchkey MK3 25 MIDI 1");
    REQUIRE (p.getMidiLearn().getCCForParam ("macro1") == 21);
    REQUIRE (p.getMidiLearn().getCCForParam (ParamID::filterCutoff) == -1);   // no longer on the pot
}

TEST_CASE ("a no-CC-map profile still passes notes through (Korg B2)", "[plugin][6c][hotplug][bug2]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    p.prepareToPlay (kSR, kBlock);
    p.loadInitPreset();   // dry sine on the live part: this checks note flow + release, not the default lead's delay tail

    // The Korg B2 factory profile is notes + damper only — its CC map is EMPTY.
    // Applying such a profile must never interfere with note handling (notes flow
    // through processBlock, not the profile). This locks in that an empty-map
    // profile does not filter/swallow note-on/off (Bug 2 hypothesis: the profile
    // blocked notes). The separate root cause — the standalone not enabling a
    // device present at launch — is verified by hand (see the wrap-up checklist).
    p.applyDeviceProfile ("Korg B2");         // empty CC map: applies nothing, harmless

    juce::MidiBuffer on; on.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
    blockRms (p, on);
    double sounding = 0.0;
    for (int i = 0; i < 4; ++i) sounding = blockRms (p, {});
    REQUIRE (sounding > 0.02);                                   // key sounds despite empty CC map

    juce::MidiBuffer off; off.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
    blockRms (p, off);
    double after = 0.0;
    for (int i = 0; i < 60; ++i) after = blockRms (p, {});
    REQUIRE (after < 0.005);                                     // note-off released it
}

TEST_CASE ("requestAllNotesOff releases a held note (unplug panic)", "[plugin][6c][hotplug][panic]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    p.apvts.getParameter ("amp_sustain")->setValueNotifyingHost (1.0f);
    p.apvts.getParameter ("amp_release")->setValueNotifyingHost (0.0f);   // snappy release
    p.prepareToPlay (kSR, kBlock);

    juce::MidiBuffer on; on.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
    blockRms (p, on);
    double sounding = 0.0;
    for (int i = 0; i < 4; ++i) sounding = blockRms (p, {});
    REQUIRE (sounding > 0.02);                                   // note is ringing

    p.requestAllNotesOff();                                      // <- hot-unplug panic
    double after = 0.0;
    for (int i = 0; i < 12; ++i) after = blockRms (p, {});
    REQUIRE (after < 0.005);                                     // fully released
}

TEST_CASE ("postToast advances the sequence and carries the message", "[plugin][6c][toast]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    const int s0 = p.toastSequence();
    p.postToast ("Launchkey Mini connected");
    REQUIRE (p.toastSequence() == s0 + 1);
    REQUIRE (p.toastMessage() == "Launchkey Mini connected");
}
