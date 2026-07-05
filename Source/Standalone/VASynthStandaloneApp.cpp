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

namespace juce
{

// Watches the standalone AudioDeviceManager and logs device / MIDI-input state.
class VASynthDeviceLogger final : private ChangeListener
{
public:
    explicit VASynthDeviceLogger (AudioDeviceManager& dmToWatch) : dm (dmToWatch)
    {
        dm.addChangeListener (this);
        logState ("audio setup");
    }
    ~VASynthDeviceLogger() override { dm.removeChangeListener (this); }

private:
    void changeListenerCallback (ChangeBroadcaster*) override { logState ("audio setup changed"); }

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

        mainWindow.reset (new StandaloneFilterWindow (
            getApplicationName(),
            LookAndFeel::getDefaultLookAndFeel().findColour (ResizableWindow::backgroundColourId),
            appProperties.getUserSettings(),
            false));   // appProperties owns the settings

        mainWindow->setVisible (true);

        // Holder + processor now exist; start logging device / MIDI state.
        deviceLogger = std::make_unique<VASynthDeviceLogger> (mainWindow->getDeviceManager());
    }

    void shutdown() override
    {
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
};

} // namespace juce

juce::JUCEApplicationBase* juce_CreateApplication() { return new juce::VASynthStandaloneApp(); }
