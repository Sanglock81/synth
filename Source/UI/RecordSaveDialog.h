// Synth — Copyright © 2026 John L Farmer. Licensed under AGPLv3; see LICENSE.
#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "VASynthLookAndFeel.h"
#include "../PluginProcessor.h"
#include "../MasterRecorder.h"

// ============================================================================
// SAVE RECORDING dialog. Opens the instant REC/STOP is pressed to stop a take: pick a
// format, pick a file, done. The take is already complete on disk (a temp WAV) by the
// time this appears, so nothing here is time-critical and nothing can lose audio.
//
// Formats come from MasterRecorder::formats(). All of them -- WAV 24/16, FLAC, Ogg Vorbis
// and MP3 -- encode inside the binary on Linux and Windows alike, so every entry in the
// picker always works and there is nothing for the user to install.
//
// Closing with Escape does NOT delete the take: the temp path is toasted so it can still
// be recovered, and the next recording (or app shutdown) cleans it up. Losing a take to a
// stray keypress would be worse than a temp file living a little longer.
// ============================================================================
class RecordSaveDialog : public juce::Component
{
public:
    explicit RecordSaveDialog (VASynthProcessor& p) : proc (p)
    {
        setWantsKeyboardFocus (false);
        auto& rec = proc.masterRecorder();

        const double secs = rec.recordedSeconds();
        title.setText ("Recorded " + timecode (secs) + " of the master output. Choose a format and save it.",
                       juce::dontSendNotification);
        title.setJustificationType (juce::Justification::topLeft);
        title.setColour (juce::Label::textColourId, VASynthLookAndFeel::dim());
        addAndMakeVisible (title);

        fmtLabel.setText ("Format:", juce::dontSendNotification);
        fmtLabel.setColour (juce::Label::textColourId, VASynthLookAndFeel::ink());   // dark panel: needs ink, not the default
        addAndMakeVisible (fmtLabel);

        auto fmts = MasterRecorder::formats();
        for (int i = 0; i < fmts.size(); ++i) format.addItem (fmts[i].label, i + 1);
        format.setSelectedId (1, juce::dontSendNotification);      // WAV 24-bit
        format.setWantsKeyboardFocus (false);
        format.onChange = [this] { refreshStatus(); };
        addAndMakeVisible (format);

        saveBtn.setButtonText ("Save as...");
        saveBtn.setWantsKeyboardFocus (false);
        saveBtn.onClick = [this] { doSave(); };
        addAndMakeVisible (saveBtn);

        discardBtn.setButtonText ("Discard");
        discardBtn.setWantsKeyboardFocus (false);
        discardBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffd8443a));
        discardBtn.onClick = [this]
        {
            proc.masterRecorder().discardTake();
            proc.postToast ("Recording discarded");
            saved = true;                    // nothing left to recover; suppress the close-toast
            closeDialog();
        };
        addAndMakeVisible (discardBtn);

        status.setJustificationType (juce::Justification::topLeft);
        status.setColour (juce::Label::textColourId, VASynthLookAndFeel::dim());
        addAndMakeVisible (status);

        // A take with dropouts is reported here too -- the dialog is the last place the user
        // sees before the file becomes "just a file".
        if (rec.droppedBlocks() > 0)
            warn = "Warning: " + juce::String (rec.droppedBlocks()) + " block(s) were dropped (disk too slow); the take has gaps.";
        refreshStatus();

        // Only reserve the status strip when there is something to say -- an empty band of dead
        // space under the buttons reads as an unfinished dialog.
        setSize (520, status.getText().isEmpty() ? 118 : 158);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16);
        title.setBounds (r.removeFromTop (34));
        r.removeFromTop (10);
        auto row = r.removeFromTop (26);
        fmtLabel.setBounds (row.removeFromLeft (56));
        format.setBounds (row.removeFromLeft (200).reduced (0, 1));
        row.removeFromLeft (14);
        saveBtn.setBounds (row.removeFromLeft (100).reduced (0, 1));
        row.removeFromLeft (6);
        discardBtn.setBounds (row.removeFromLeft (80).reduced (0, 1));
        r.removeFromTop (8);
        status.setBounds (r);                      // whatever is left: 0, 1 or 2 lines
    }

    static void show (VASynthProcessor& proc, juce::Component* parent, std::function<void()> onClose)
    {
        auto dlg = std::make_unique<RecordSaveDialog> (proc);
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned (dlg.release());
        o.dialogTitle = "Save Recording";
        o.dialogBackgroundColour = VASynthLookAndFeel::panel();
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = false;
        if (parent != nullptr) o.componentToCentreAround = parent;
        if (auto* w = o.launchAsync())
            w->enterModalState (true, juce::ModalCallbackFunction::create ([onClose] (int) { if (onClose) onClose(); }), false);
    }

    // Test seam: drive the dialog headlessly (no native file chooser). Transcodes the take
    // to `dest` in the currently selected format and returns the outcome.
    bool saveToForTest (const juce::File& dest, juce::String& error)
    {
        return proc.masterRecorder().transcodeTo (dest, selectedFormat(), error);
    }
    juce::ComboBox& formatBox() noexcept { return format; }
    juce::TextButton& discardButton() noexcept { return discardBtn; }
    juce::String statusText() const { return status.getText(); }

    ~RecordSaveDialog() override
    {
        // Closed without saving (Escape / the title-bar X): keep the take and say where it is.
        if (! saved && proc.masterRecorder().takeFile().existsAsFile())
            proc.postToast ("Take kept at " + proc.masterRecorder().takeFile().getFullPathName());
    }

private:
    MasterRecorder::Format selectedFormat() const
    {
        auto fmts = MasterRecorder::formats();
        return fmts[juce::jlimit (0, fmts.size() - 1, format.getSelectedId() - 1)];
    }

    // "1:23.4" — a length the user can match against what they played.
    static juce::String timecode (double secs)
    {
        const int m = (int) (secs / 60.0);
        return juce::String (m) + ":" + juce::String (secs - m * 60.0, 1).paddedLeft ('0', 4);
    }

    // The only thing the status strip carries up front is a dropout warning -- with the
    // encoder embedded there is no "install something" case for any format.
    void refreshStatus()
    {
        const bool had = status.getText().isNotEmpty();
        status.setText (warn, juce::dontSendNotification);
        if (had != warn.isNotEmpty() && getWidth() > 0) setSize (getWidth(), warn.isNotEmpty() ? 158 : 118);
    }

    void doSave()
    {
        const auto fmt = selectedFormat();
        const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d-%H%M%S");
        const auto suggested = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                                   .getChildFile ("synth-take-" + stamp + "." + fmt.ext);
        chooser = std::make_unique<juce::FileChooser> ("Save the recording",
                                                       suggested, "*." + juce::String (fmt.ext));
        chooser->launchAsync (juce::FileBrowserComponent::saveMode
                            | juce::FileBrowserComponent::canSelectFiles
                            | juce::FileBrowserComponent::warnAboutOverwriting,
            [this, fmt] (const juce::FileChooser& fc)
            {
                auto dest = fc.getResult();
                if (dest == juce::File()) return;                       // cancelled; take stays
                if (dest.getFileExtension().isEmpty())
                    dest = dest.withFileExtension (fmt.ext);            // honour the picked format

                status.setText ("Encoding...", juce::dontSendNotification);
                repaint();
                juce::String error;
                if (proc.masterRecorder().transcodeTo (dest, fmt, error))
                {
                    proc.masterRecorder().discardTake();                // the temp WAV has served its purpose
                    saved = true;
                    proc.postToast ("Saved " + dest.getFileName());
                    closeDialog();
                }
                else
                {
                    status.setText (error, juce::dontSendNotification);
                    proc.postToast ("Save failed");
                }
            });
    }

    void closeDialog()
    {
        if (auto* w = findParentComponentOfClass<juce::DialogWindow>()) w->exitModalState (0);
    }

    VASynthProcessor& proc;
    juce::Label title, fmtLabel, status;
    juce::ComboBox format;
    juce::TextButton saveBtn, discardBtn;
    juce::String warn;
    bool saved = false;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RecordSaveDialog)
};
