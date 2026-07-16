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

// Plug-and-play MIDI: auto-connect inputs, apply their device profile, toast on
// connect/disconnect, and panic (all-notes-off) on unplug so a note can't hang.
// Uses JUCE 8's MidiDeviceListConnection for device-list changes AND listens to
// the AudioDeviceManager for setup changes.
//
// Bug B (input reliability): every present controller must stay a playing surface
// across the app's lifecycle. Enabling an input at startup fixed the "present at
// launch" case (Bug 2), but the Audio/MIDI settings dialog (and audio-device
// changes) can silently DISABLE an input afterwards — the notes then stop with no
// obvious cause. So we re-assert enablement on every device-manager change, not
// just on hot-plug. (Enabling also routes: the holder registers its player as an
// all-device MIDI callback, so an enabled input reaches the processor.)
class VASynthMidiHotplug : private ChangeListener,
                           public  MidiInputCallback
{
public:
    VASynthMidiHotplug (AudioDeviceManager& dmToUse, ::VASynthProcessor& procToUse)
        : dm (dmToUse), proc (procToUse)
    {
        known = MidiInput::getAvailableDevices();
        for (auto& d : known) proc.applyDeviceProfile (d.name);   // profile all present
        ensureAllInputsEnabled();                                 // input contract: all play
        dm.addChangeListener (this);
        connection = MidiDeviceListConnection::make ([this] { refresh(); });
    }

    ~VASynthMidiHotplug() override { dm.removeChangeListener (this); }

    // 7C: per-input capture. We are registered as the all-device MIDI callback (in
    // place of the holder's player), so every enabled input's MIDI arrives here
    // tagged with its source device. We route it to the device's assigned part — one
    // synth per part, "multiple synths into one pedalboard". No double-trigger: the
    // player's merge is removed (see the app's initialise()), so this is the ONLY
    // path for hardware MIDI. Runs on the MIDI thread (non-audio); routeMidi pushes
    // into the processor's lock-free FIFO.
    void handleIncomingMidiMessage (MidiInput* source, const MidiMessage& message) override
    {
        const auto name = source != nullptr ? source->getName() : juce::String();
        proc.routeSurfaceMessage (name, message);    // zone-resolves notes (+ transpose); bumps activity
    }

private:
    static bool containsId (const Array<MidiDeviceInfo>& list, const String& id)
    {
        for (auto& d : list) if (d.identifier == id) return true;
        return false;
    }

    // Enable every present MIDI input that isn't already enabled (and profile the
    // ones we newly enable). Idempotent: acts only on disabled devices, so the
    // ChangeListener re-entrancy settles in one extra (no-op) pass.
    void ensureAllInputsEnabled()
    {
        const auto present = MidiInput::getAvailableDevices();
        StringArray presentIds, enabledIds;
        for (auto& d : present)
        {
            presentIds.add (d.identifier);
            if (dm.isMidiInputDeviceEnabled (d.identifier)) enabledIds.add (d.identifier);
        }
        for (auto& id : AudioDeviceCuration::inputsNeedingEnable (presentIds, enabledIds))
        {
            dm.setMidiInputDeviceEnabled (id, true);
            for (auto& d : present)
                if (d.identifier == id)
                {
                    proc.applyDeviceProfile (d.name);
                    Logger::writeToLog ("MIDI input (re)enabled to keep it playing: '" + d.name + "'");
                }
        }
    }

    // AudioDeviceManager setup changed (device switch, or a settings-dialog MIDI
    // toggle) — re-assert that every present controller is enabled.
    void changeListenerCallback (ChangeBroadcaster*) override { ensureAllInputsEnabled(); }

    void refresh()
    {
        auto avail = MidiInput::getAvailableDevices();

        for (auto& d : avail)                              // newly connected
            if (! containsId (known, d.identifier))
            {
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
        ensureAllInputsEnabled();                          // enable any newcomers
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

        // Add a MAXIMISE (full-screen) button to the window title bar, next to the
        // minimise + close buttons. StandaloneFilterWindow ships with only minimise +
        // close, so there was no title-bar way to fill the screen; the maximise button
        // toggles full-screen (fills the display) via DocumentWindow.
        mainWindow->setTitleBarButtonsRequired (
            DocumentWindow::minimiseButton | DocumentWindow::maximiseButton | DocumentWindow::closeButton,
            false);

        mainWindow->setVisible (true);
        // Default to full-screen (the recommended live mode): no OS title bar to fight, so a
        // touch drag on a knob near the top edge can't be grabbed as a window move. The
        // maximise button still toggles back to a window.
        mainWindow->setFullScreen (true);

        // Holder + processor now exist; start logging device / MIDI state, and
        // start the plug-and-play MIDI watcher.
        deviceLogger = std::make_unique<VASynthDeviceLogger> (mainWindow->getDeviceManager());
        if (auto* va = dynamic_cast<::VASynthProcessor*> (mainWindow->pluginHolder->processor.get()))
        {
            auto& dm = mainWindow->getDeviceManager();
            midiHotplug = std::make_unique<VASynthMidiHotplug> (dm, *va);

            // 7C per-input routing: replace the holder player's all-device MIDI merge
            // (which would send every device's notes to the live part) with our
            // routing callback, so each surface reaches its assigned part and there
            // is exactly ONE path for hardware MIDI (no double-trigger). QWERTY flows
            // through its own "QWERTY" surface zones (routeSurfaceMessage) the same way.
            dm.removeMidiInputDeviceCallback ({}, &mainWindow->pluginHolder->player);
            dm.addMidiInputDeviceCallback    ({}, midiHotplug.get());
        }
    }

    void shutdown() override
    {
        if (midiHotplug != nullptr && mainWindow != nullptr)
            mainWindow->getDeviceManager().removeMidiInputDeviceCallback ({}, midiHotplug.get());
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
