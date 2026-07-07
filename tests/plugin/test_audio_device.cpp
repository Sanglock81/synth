// ============================================================================
// Audio device flexibility + fallback (Item A). What's testable headlessly:
//   * ALSA/JACK device types are compiled in -> the standalone's Audio/MIDI
//     Settings can offer device-type selection.
//   * When a saved/preferred output device is absent (e.g. the Scarlett is
//     unplugged), AudioDeviceManager falls back to a working default rather than
//     opening nothing (silent) — this mirrors the standalone's
//     initialise(..., selectDefaultDeviceOnFailure = true, ...).
//
// Opening real hardware may not be possible in a headless CI sandbox; those
// cases are reported (WARN) rather than failed. End-to-end verification (built-
// in audio output, GUI type switching, real Scarlett-absent launch) needs hands.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <juce_audio_utils/juce_audio_utils.h>
#include <vector>

TEST_CASE ("ALSA/JACK device types are available (type selection possible)", "[plugin][audio][device]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::AudioDeviceManager dm;

    juce::OwnedArray<juce::AudioIODeviceType> types;
    dm.createAudioDeviceTypes (types);

    juce::StringArray names;
    for (auto* t : types) names.add (t->getTypeName());
    INFO ("device types: " << names.joinIntoString (", "));

    REQUIRE (! types.isEmpty());
    // At least one native backend must be present for device selection. The name
    // is platform-specific: ALSA/JACK on Linux, Windows Audio/DirectSound on
    // Windows, CoreAudio on macOS.
   #if JUCE_LINUX || JUCE_BSD
    REQUIRE (names.contains ("ALSA"));
   #elif JUCE_WINDOWS
    REQUIRE ((names.contains ("Windows Audio") || names.contains ("DirectSound")));
   #elif JUCE_MAC
    REQUIRE (names.contains ("CoreAudio"));
   #endif
}

TEST_CASE ("device manager falls back to default when the saved device is absent",
           "[plugin][audio][device][fallback]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::AudioDeviceManager dm;

    // Saved state naming a device that isn't present (the Scarlett, unplugged).
    const char* xmlText =
        "<DEVICESETUP audioDeviceName=\"VA-Synth-Absent-Scarlett-XYZ\" "
        "audioOutputDeviceName=\"VA-Synth-Absent-Scarlett-XYZ\" "
        "audioInputDeviceName=\"\" audioDeviceRate=\"48000\"/>";
    auto saved = juce::parseXML (xmlText);

    // selectDefaultDeviceOnFailure = true — exactly what the standalone passes.
    const juce::String err = dm.initialise (0, 2, saved.get(), true);

    auto* dev = dm.getCurrentAudioDevice();
    if (dev == nullptr)
    {
        WARN ("no openable audio output device in this environment ('" << err
              << "') — fallback not exercisable headlessly; verify on the ThinkPad.");
        SUCCEED();
        return;
    }

    INFO ("fell back to: " << dev->getName());
    REQUIRE (dev->getName() != "VA-Synth-Absent-Scarlett-XYZ");   // did NOT open the absent device
    REQUIRE (dev->getName().isNotEmpty());                        // opened *something* (not silent)
}
