#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "VASynthLookAndFeel.h"
#include "../PluginProcessor.h"

// ============================================================================
// SESSION EXPORT dialog (#98). A DAW-handoff BOUNCE: choose a folder and a bar count (default = the
// scene's realign cycle), and the session is rendered OFFLINE into per-part WAV stems + per-part
// MIDI + master.wav + manifest.json. The live audio thread is SUSPENDED (to silence) around the
// bounce so the offline render never races it. Same modal discipline as the OUTPUTS/INPUTS dialogs.
// ============================================================================
class SessionExportDialog : public juce::Component
{
public:
    explicit SessionExportDialog (VASynthProcessor& p) : proc (p)
    {
        setWantsKeyboardFocus (false);

        title.setText ("Bounce the session to a folder for your DAW: per-part WAV stems + MIDI, the "
                       "master WAV, and a manifest. Stops audio briefly while it renders.",
                       juce::dontSendNotification);
        title.setJustificationType (juce::Justification::topLeft);
        title.setColour (juce::Label::textColourId, VASynthLookAndFeel::dim());
        addAndMakeVisible (title);

        barsLabel.setText ("Bars:", juce::dontSendNotification);
        addAndMakeVisible (barsLabel);
        bars.setText (juce::String (juce::jmax (1, proc.realignBars())), juce::dontSendNotification);
        bars.setInputRestrictions (3, "0123456789");
        addAndMakeVisible (bars);

        exportBtn.setButtonText ("Export to folder...");
        exportBtn.setWantsKeyboardFocus (false);
        exportBtn.onClick = [this] { doExport(); };
        addAndMakeVisible (exportBtn);

        status.setJustificationType (juce::Justification::centredLeft);
        status.setColour (juce::Label::textColourId, VASynthLookAndFeel::dim());
        addAndMakeVisible (status);

        setSize (480, 156);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16);
        title.setBounds (r.removeFromTop (48));
        r.removeFromTop (10);
        auto row = r.removeFromTop (26);
        barsLabel.setBounds (row.removeFromLeft (44));
        bars.setBounds (row.removeFromLeft (70));
        row.removeFromLeft (14);
        exportBtn.setBounds (row.removeFromLeft (170));
        r.removeFromTop (10);
        status.setBounds (r.removeFromTop (20));
    }

    static void show (VASynthProcessor& proc, juce::Component* parent, std::function<void()> onClose)
    {
        auto dlg = std::make_unique<SessionExportDialog> (proc);
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned (dlg.release());
        o.dialogTitle = "Export Session";
        o.dialogBackgroundColour = VASynthLookAndFeel::panel();
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = false;
        if (parent != nullptr) o.componentToCentreAround = parent;
        if (auto* w = o.launchAsync())
            w->enterModalState (true, juce::ModalCallbackFunction::create ([onClose] (int) { if (onClose) onClose(); }), false);
    }

private:
    void doExport()
    {
        const int nbars = juce::jlimit (1, 256, bars.getText().getIntValue());
        chooser = std::make_unique<juce::FileChooser> ("Choose a folder for the session bounce",
                                                       juce::File::getSpecialLocation (juce::File::userMusicDirectory));
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectDirectories,
            [this, nbars] (const juce::FileChooser& fc)
            {
                const auto dir = fc.getResult();
                if (dir == juce::File()) return;
                status.setText ("Rendering " + juce::String (nbars) + " bar(s)...", juce::dontSendNotification);
                repaint();
                proc.setAudioSuspended (true);
                juce::Thread::sleep (60);                    // let any in-flight audio block finish before we render
                const bool ok = proc.bounceSession (dir, nbars);
                proc.setAudioSuspended (false);
                status.setText (ok ? "Exported to " + dir.getFileName() : "Export failed", juce::dontSendNotification);
                proc.postToast (ok ? "Session exported" : "Session export failed");
            });
    }

    VASynthProcessor& proc;
    juce::Label title, barsLabel, status;
    juce::TextEditor bars;
    juce::TextButton exportBtn;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SessionExportDialog)
};
