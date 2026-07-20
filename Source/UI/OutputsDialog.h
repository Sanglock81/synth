#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include "VASynthLookAndFeel.h"
#include "../PluginProcessor.h"

// ============================================================================
// OUTPUTS dialog (#85). MIDI clock transmit: an enable toggle (the APVTS clock_out param) and, in
// the STANDALONE, a picker for the MIDI output device the 24-ppq clock is sent to (so an Aeros /
// Chase Bliss rig locks to the synth's tempo). In a plugin the clock rides the host's MIDI output,
// so only the toggle is shown. Same modal discipline as the INPUTS / Save dialogs (no focus leak).
// ============================================================================
class OutputsDialog : public juce::Component,
                      private juce::Timer
{
public:
    explicit OutputsDialog (VASynthProcessor& p) : proc (p)
    {
        setWantsKeyboardFocus (false);

        enable.setButtonText ("Transmit MIDI clock (24 ppq + start/stop)");
        enable.setWantsKeyboardFocus (false);
        enableAtt = std::make_unique<juce::ButtonParameterAttachment> (*proc.apvts.getParameter (ParamID::clockOut), enable);
        addAndMakeVisible (enable);

        info.setJustificationType (juce::Justification::topLeft);
        info.setColour (juce::Label::textColourId, VASynthLookAndFeel::dim());
        addAndMakeVisible (info);

        standalone = (proc.wrapperType == juce::AudioProcessor::wrapperType_Standalone);
        if (standalone)
        {
            deviceBox.setWantsKeyboardFocus (false);
            deviceBox.onChange = [this] { chooseSelected(); };
            addAndMakeVisible (deviceBox);
            refreshDevices();
            info.setText ("Send the clock to this MIDI output. Set your looper/pedals to EXTERNAL / MIDI clock.",
                          juce::dontSendNotification);
        }
        else
            info.setText ("The clock is sent on the plugin's MIDI output — route it to your gear in the host.",
                          juce::dontSendNotification);

        setSize (460, standalone ? 150 : 110);
        startTimerHz (2);   // keep the device list fresh (hot-plug)
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16);
        enable.setBounds (r.removeFromTop (28));
        r.removeFromTop (8);
        if (standalone)
        {
            auto row = r.removeFromTop (26);
            row.removeFromLeft (4);
            deviceBox.setBounds (row);
            r.removeFromTop (8);
        }
        info.setBounds (r);
    }

    static void show (VASynthProcessor& proc, juce::Component* parent, std::function<void()> onClose)
    {
        auto dlg = std::make_unique<OutputsDialog> (proc);
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned (dlg.release());
        o.dialogTitle = "Outputs";
        o.dialogBackgroundColour = VASynthLookAndFeel::panel();
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = false;
        if (parent != nullptr) o.componentToCentreAround = parent;
        if (auto* w = o.launchAsync())
            w->enterModalState (true, juce::ModalCallbackFunction::create ([onClose] (int) { if (onClose) onClose(); }), false);
    }

private:
    void refreshDevices()
    {
        auto devices = juce::MidiOutput::getAvailableDevices();
        if (devices.size() == lastCount && deviceBox.getNumItems() > 0) return;   // no change
        lastCount = devices.size();

        const auto want = proc.requestedClockDeviceId_();
        deviceBox.clear (juce::dontSendNotification);
        ids.clearQuick();
        deviceBox.addItem ("None", 1); ids.add ({});
        int selectId = 1;
        for (int i = 0; i < devices.size(); ++i)
        {
            deviceBox.addItem (devices[i].name, i + 2);
            ids.add (devices[i].identifier);
            if (devices[i].identifier == want) selectId = i + 2;
        }
        deviceBox.setSelectedId (selectId, juce::dontSendNotification);
    }

    void chooseSelected()
    {
        const int idx = deviceBox.getSelectedId() - 1;   // 0 = None
        proc.setRequestedClockDeviceId (idx >= 0 && idx < ids.size() ? ids[idx] : juce::String());
    }

    void timerCallback() override { if (standalone) refreshDevices(); }

    VASynthProcessor& proc;
    bool standalone = false;
    juce::ToggleButton enable;
    std::unique_ptr<juce::ButtonParameterAttachment> enableAtt;
    juce::ComboBox deviceBox;
    juce::StringArray ids;              // parallel to deviceBox items (index 0 = None)
    int lastCount = -1;
    juce::Label info;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OutputsDialog)
};
