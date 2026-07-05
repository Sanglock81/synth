#pragma once
#include "PluginProcessor.h"
#include "QwertyKeyboard.h"
#include "Observability/DebugOverlay.h"

// ============================================================================
// v1 GUI: wraps JUCE's GenericAudioProcessorEditor (a control per APVTS param),
// plus computer-keyboard (QWERTY) note input for the standalone.
//
// QWERTY design (see QwertyKeyboard.h for the layout):
//  * keyStateChanged + per-key edge detection (KeyPress::isKeyCurrentlyDown),
//    so OS auto-repeat is ignored — one note-on per press, one note-off per
//    release.
//  * Notes feed the processor's MidiKeyboardState, merged into the MIDI buffer
//    in processBlock, so they share the exact hardware-MIDI engine path.
//  * All-notes-off when our window loses keyboard focus (Alt-Tab / click-away)
//    and on close — no stuck notes.
//  * Never captured while a text field has focus (param value entry, settings).
//  * Standalone only; in a plugin the host owns the keyboard (feature disabled,
//    build unaffected).
//
// v2: replace with a custom Component layout + right-click MIDI-learn. Nothing
// in the engine/processor changes when we do.
// ============================================================================

class VASynthEditor : public juce::AudioProcessorEditor,
                      private juce::Timer
{
public:
    explicit VASynthEditor (VASynthProcessor& p)
        : AudioProcessorEditor (p), proc (p), genericEditor (p)
    {
        addAndMakeVisible (genericEditor);
        addChildComponent (overlay);        // hidden until F12
        setSize (520, 700);
        setResizable (true, true);

        if (isStandalone())
        {
            setWantsKeyboardFocus (true);   // so keyStateChanged reaches us
            startTimerHz (30);              // focus-loss watchdog
        }
    }

    ~VASynthEditor() override
    {
        stopTimer();
        allNotesOff();                      // no stuck notes on close
    }

    void resized() override
    {
        genericEditor.setBounds (getLocalBounds());
        overlay.setBounds (getLocalBounds().removeFromTop (60).removeFromRight (300).reduced (4));
    }

    // F12 toggles the debug overlay (works standalone and in a plugin).
    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::F12Key)
        {
            overlay.setVisible (! overlay.isVisible());
            return true;
        }
        return false;
    }

    void parentHierarchyChanged() override
    {
        if (isStandalone() && isShowing())
            grabKeyboardFocus();
    }

    bool keyStateChanged (bool) override
    {
        if (! isStandalone())
            return false;                   // host owns the keyboard in a plugin

        // Never steal keys while a text field is focused (param value entry,
        // audio/MIDI settings). Release anything held so notes don't stick.
        if (auto* f = juce::Component::getCurrentlyFocusedComponent())
            if (dynamic_cast<juce::TextEditor*> (f) != nullptr)
            {
                allNotesOff();
                return false;
            }

        qwerty.update ([] (int kc) { return juce::KeyPress::isKeyCurrentlyDown (kc); },
                       [this] (int note, bool on) { emitNote (note, on); });
        return qwerty.anyHeld();
    }

private:
    bool isStandalone() const
    {
        return proc.wrapperType == juce::AudioProcessor::wrapperType_Standalone;
    }

    void timerCallback() override
    {
        // All-notes-off when our window loses keyboard focus, regardless of which
        // child held it (hasKeyboardFocus(true) includes children).
        const bool focused = hasKeyboardFocus (true);
        if (hadFocus && ! focused)
            allNotesOff();
        hadFocus = focused;
    }

    void emitNote (int note, bool on)
    {
        if (on) proc.qwertyKeyboardState.noteOn  (1, note, 0.8f);   // fixed velocity 0.8
        else    proc.qwertyKeyboardState.noteOff (1, note, 0.0f);
    }

    void allNotesOff() { qwerty.releaseAll ([this] (int note, bool on) { emitNote (note, on); }); }

    VASynthProcessor& proc;
    juce::GenericAudioProcessorEditor genericEditor;
    DebugOverlay overlay { proc.health };
    QwertyKeyboard qwerty;
    bool hadFocus = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VASynthEditor)
};
