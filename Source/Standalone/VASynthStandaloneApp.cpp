// ============================================================================
// Custom standalone app (compiled ONLY into the Standalone target, which sets
// JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP). It mirrors JUCE's default
// StandaloneFilterApp — same PropertiesFile-backed audio-settings persistence —
// and adds observability: it logs the selected audio device + type and the
// enabled MIDI inputs at startup, and re-logs on any device/MIDI change.
//
// Device info lives in the standalone's AudioDeviceManager (owned by the plugin
// holder), which the shared plugin code can't reach — hence this app-level hook.
// Logs go through juce::Logger, which the processor's AudioHealthLogger has
// registered onto the app log file.
// ============================================================================
#include <juce_audio_utils/juce_audio_utils.h>          // AudioDeviceManager, MidiInput, GUI
#include <juce_audio_plugin_client/juce_audio_plugin_client.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#include "../PluginProcessor.h"                          // VASynthProcessor (profile/panic/toast)
#include "../AppInfo.h"                                   // rename + config migration
#include "AudioDeviceCuration.h"                          // Bug 1: device-list policy

namespace juce
{

// All output-device names the ALSA/JACK backends expose right now (message
// thread; opens nothing — just scans). Used to pick a sane default and to log a
// curated menu so the user isn't staring at a wall of raw ALSA aliases (Bug 1).
static StringArray availableOutputDeviceNames()
{
    AudioDeviceManager probe;
    OwnedArray<AudioIODeviceType> types;
    probe.createAudioDeviceTypes (types);
    StringArray names;
    for (auto* t : types)
    {
        t->scanForDevices();
        names.addArray (t->getDeviceNames (false));      // false = outputs
    }
    return names;
}

// The "just works" default output: the PipeWire/default endpoint when present, so
// the synth follows the OS default sink (route the Scarlett there and it plays).
static String preferredDefaultOutput()
{
    return AudioDeviceCuration::pickPreferredDeviceName (availableOutputDeviceNames());
}

// Plug-and-play MIDI: auto-connect newly-appeared inputs, apply their device
// profile, toast on connect/disconnect, and panic (all-notes-off) on unplug so a
// note can't hang. Uses JUCE 8's MidiDeviceListConnection for change callbacks.
class VASynthMidiHotplug
{
public:
    VASynthMidiHotplug (AudioDeviceManager& dmToUse, ::VASynthProcessor& procToUse)
        : dm (dmToUse), proc (procToUse)
    {
        known = MidiInput::getAvailableDevices();
        // Devices already present at launch (the normal live case: gear is plugged
        // in BEFORE the app starts). On desktop JUCE's autoOpenMidiDevices defaults
        // to false, so nothing else enables these — we must. Enabling ALSO routes
        // them: the plugin holder registers its player as an all-device MIDI
        // callback, so an enabled input's notes reach the processor. Applying the
        // profile alone (the old behaviour) recognised the device but left its
        // input disabled — keys did nothing (Bug 2). Silent at launch (no toast per
        // device); the settings dialog still reflects the enabled state.
        for (auto& d : known)
        {
            dm.setMidiInputDeviceEnabled (d.identifier, true);
            proc.applyDeviceProfile (d.name);
        }
        connection = MidiDeviceListConnection::make ([this] { refresh(); });
    }

private:
    static bool containsId (const Array<MidiDeviceInfo>& list, const String& id)
    {
        for (auto& d : list) if (d.identifier == id) return true;
        return false;
    }

    void refresh()
    {
        auto avail = MidiInput::getAvailableDevices();

        for (auto& d : avail)                              // newly connected
            if (! containsId (known, d.identifier))
            {
                dm.setMidiInputDeviceEnabled (d.identifier, true);
                proc.applyDeviceProfile (d.name);
                proc.postToast (d.name + " connected");
                Logger::writeToLog ("MIDI hot-plug: connected '" + d.name + "'");
            }

        for (auto& d : known)                              // disconnected
            if (! containsId (avail, d.identifier))
            {
                proc.requestAllNotesOff();
                proc.postToast (d.name + " disconnected");
                Logger::writeToLog ("MIDI hot-plug: disconnected '" + d.name + "'");
            }

        known = avail;
    }

    AudioDeviceManager&      dm;
    ::VASynthProcessor&      proc;
    Array<MidiDeviceInfo>    known;
    MidiDeviceListConnection connection;
};

// Watches the standalone AudioDeviceManager and logs device / MIDI-input state.
class VASynthDeviceLogger final : private ChangeListener
{
public:
    explicit VASynthDeviceLogger (AudioDeviceManager& dmToWatch) : dm (dmToWatch)
    {
        dm.addChangeListener (this);
        logAvailableOutputs();      // once at startup: the curated menu of choices
        logState ("audio setup");
    }
    ~VASynthDeviceLogger() override { dm.removeChangeListener (this); }

private:
    void changeListenerCallback (ChangeBroadcaster*) override { logState ("audio setup changed"); }

    // Log the curated output menu (friendly endpoints + card names) plus the raw
    // count, so the log tells the user which device to pick — and records what the
    // full ALSA wall looked like — without opening anything.
    void logAvailableOutputs()
    {
        const auto all     = availableOutputDeviceNames();
        const auto curated = AudioDeviceCuration::curateDeviceList (all);
        Logger::writeToLog ("audio outputs (curated " + String (curated.size()) + " of "
                            + String (all.size()) + " raw): [" + curated.joinIntoString (", ") + "]");
        Logger::writeToLog ("audio output default (just-works): '" + preferredDefaultOutput()
                            + "'  (Settings dialog shows the full raw list as the advanced view)");
    }

    void logState (const String& reason)
    {
        String msg (reason + ": ");
        if (auto* dev = dm.getCurrentAudioDevice())
            msg << "device='" << dev->getName() << "' type='" << dev->getTypeName()
                << "' rate=" << String (dev->getCurrentSampleRate(), 0)
                << " buffer=" << dev->getCurrentBufferSizeSamples();
        else
            msg << "no audio device open";

        StringArray midiIns;
        for (auto& in : MidiInput::getAvailableDevices())
            if (dm.isMidiInputDeviceEnabled (in.identifier))
                midiIns.add (in.name);
        msg << "  midi-in=[" << midiIns.joinIntoString (", ") << "]";

        Logger::writeToLog (msg);
    }

    AudioDeviceManager& dm;
};

//==============================================================================
class VASynthStandaloneApp final : public JUCEApplication
{
public:
    VASynthStandaloneApp()
    {
        PropertiesFile::Options options;
        options.applicationName     = CharPointer_UTF8 (JucePlugin_Name);
        options.filenameSuffix      = ".settings";
        options.osxLibrarySubFolder = "Application Support";
       #if JUCE_LINUX || JUCE_BSD
        options.folderName          = "~/.config";
       #else
        options.folderName          = "";
       #endif
        // One-time: carry the pre-rename "VA Synth.settings" (audio/MIDI device
        // choice) over to "synth.settings" so an existing rig keeps its setup.
        AppInfo::migrateLegacySettings (options.getDefaultFile().getParentDirectory());
        appProperties.setStorageParameters (options);
    }

    const String getApplicationName() override           { return CharPointer_UTF8 (JucePlugin_Name); }
    const String getApplicationVersion() override        { return JucePlugin_VersionString; }
    bool moreThanOneInstanceAllowed() override           { return true; }
    void anotherInstanceStarted (const String&) override {}

    void initialise (const String&) override
    {
        if (Desktop::getInstance().getDisplays().displays.isEmpty())
        {
            jassertfalse;   // no display -> no window
            return;
        }

        // Bug 1: hand the holder a preferred default OUTPUT device — the
        // PipeWire/default endpoint — so a first run (no saved audio setup) opens
        // something that actually makes sound and follows the OS default sink
        // (select the Scarlett there and it plays). A previously-saved device is
        // still honoured; this only picks the default when none is stored.
        const String preferredOut = preferredDefaultOutput();
        Logger::writeToLog ("startup: preferred default output = '" + preferredOut + "'");

        mainWindow.reset (new StandaloneFilterWindow (
            getApplicationName(),
            LookAndFeel::getDefaultLookAndFeel().findColour (ResizableWindow::backgroundColourId),
            appProperties.getUserSettings(),
            false,                 // appProperties owns the settings
            preferredOut));        // preferredDefaultDeviceName (Bug 1: just-works output)

        mainWindow->setVisible (true);

        // Holder + processor now exist; start logging device / MIDI state, and
        // start the plug-and-play MIDI watcher.
        deviceLogger = std::make_unique<VASynthDeviceLogger> (mainWindow->getDeviceManager());
        if (auto* va = dynamic_cast<::VASynthProcessor*> (mainWindow->pluginHolder->processor.get()))
            midiHotplug = std::make_unique<VASynthMidiHotplug> (mainWindow->getDeviceManager(), *va);
    }

    void shutdown() override
    {
        midiHotplug  = nullptr;
        deviceLogger = nullptr;
        mainWindow   = nullptr;
        appProperties.saveIfNeeded();
    }

    void systemRequestedQuit() override
    {
        if (mainWindow != nullptr)
            mainWindow->pluginHolder->savePluginState();
        quit();
    }

private:
    ApplicationProperties appProperties;
    std::unique_ptr<StandaloneFilterWindow> mainWindow;
    std::unique_ptr<VASynthDeviceLogger>    deviceLogger;
    std::unique_ptr<VASynthMidiHotplug>     midiHotplug;
};

} // namespace juce

juce::JUCEApplicationBase* juce_CreateApplication() { return new juce::VASynthStandaloneApp(); }
