#pragma once
#include "PluginProcessor.h"
#include "QwertyKeyboard.h"
#include "Observability/DebugOverlay.h"
#include "UI/VASynthLookAndFeel.h"
#include "UI/Widgets.h"
#include "PresetManager.h"

// ============================================================================
// Hardware-style custom editor. One surface, panel sections left-to-right in
// signal-flow order, everything visible at once (no tabs/pages). Vertical
// faders + segmented buttons bound to the APVTS via attachments; MIDI-learn on
// every control (right-click / long-press). Dark hardware LookAndFeel.
//
// Standalone: F11 fullscreen. F12 debug overlay (both formats). QWERTY note
// input preserved (every control refuses keyboard focus). Layout is FlexBox-
// based and scales with the window — no hardcoded pixel positions.
// ============================================================================

class VASynthEditor : public juce::AudioProcessorEditor,
                      private juce::Timer
{
public:
    explicit VASynthEditor (VASynthProcessor& p)
        : AudioProcessorEditor (p), proc (p), presets (p.apvts)
    {
        setLookAndFeel (&lnf);

        buildSections();
        addChildComponent (overlay);

        setResizable (true, true);
        setResizeLimits (900, 480, 3000, 1600);

        // Default startup size (20% wider than the original 1180), clamped to the
        // display so it never opens off-screen on a smaller monitor.
        int w = 1416, h = 620;
        if (auto* disp = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        {
            const auto area = disp->userArea;
            w = juce::jmin (w, area.getWidth()  - 40);
            h = juce::jmin (h, area.getHeight() - 40);
        }
        setSize (w, h);

        if (isStandalone())
        {
            setWantsKeyboardFocus (true);
            startTimerHz (30);            // focus-loss watchdog
        }
    }

    ~VASynthEditor() override
    {
        stopTimer();
        allNotesOff();
        setLookAndFeel (nullptr);
    }

    void paint (juce::Graphics& g) override { g.fillAll (VASynthLookAndFeel::panel()); }

    // Clicking the panel background returns keyboard focus to the editor so
    // QWERTY resumes (controls refuse focus, so this only fires on the backdrop).
    void mouseDown (const juce::MouseEvent&) override { grabQwertyFocus(); }

    // Deterministically own keyboard focus once we're actually on screen — not
    // in the constructor (too early). Async re-grab defeats any startup race.
    void parentHierarchyChanged() override { grabQwertyFocus(); }
    void visibilityChanged() override      { grabQwertyFocus(); }

    // Restores QWERTY focus after a transient dialog (e.g. Save-preset) closes.
    void restoreQwertyFocus() { grabQwertyFocus(); }

    void resized() override
    {
        overlay.setBounds (getWidth() - 320, 8, 312, 58);

        auto area = getLocalBounds().reduced (6);
        juce::FlexBox row;
        row.flexDirection = juce::FlexBox::Direction::row;
        for (auto& s : sections)
            row.items.add (juce::FlexItem (*s.panel).withFlex (s.flex).withMargin (3.0f));
        row.performLayout (area);
    }

    // F11 fullscreen (standalone), F12 debug overlay.
    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::F12Key) { overlay.setVisible (! overlay.isVisible()); return true; }
        if (key == juce::KeyPress::F11Key && isStandalone()) { toggleFullscreen(); return true; }
        return false;
    }

    bool keyStateChanged (bool) override
    {
        if (! isStandalone()) return false;
        if (auto* f = juce::Component::getCurrentlyFocusedComponent())
            if (dynamic_cast<juce::TextEditor*> (f) != nullptr) { allNotesOff(); return false; }
        qwerty.update ([] (int kc) { return juce::KeyPress::isKeyCurrentlyDown (kc); },
                       [this] (int note, bool on) { emitNote (note, on); });
        return qwerty.anyHeld();
    }

private:
    struct SectionEntry { std::unique_ptr<Section> panel; float flex; };

    bool isStandalone() const { return proc.wrapperType == juce::AudioProcessor::wrapperType_Standalone; }

    void grabQwertyFocus()
    {
        if (! isStandalone() || ! isShowing()) return;
        grabKeyboardFocus();
        // Re-grab after the message loop settles, to win any startup focus race
        // (e.g. a dialog or child that momentarily grabbed focus).
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<VASynthEditor> (this)]
        {
            if (safe != nullptr && safe->isShowing()) safe->grabKeyboardFocus();
        });
    }

    Section& addSection (juce::String title, juce::Colour tint, float flex)
    {
        auto s = std::make_unique<Section> (std::move (title), tint);
        addAndMakeVisible (*s);
        auto& ref = *s;
        sections.push_back ({ std::move (s), flex });
        return ref;
    }

    void addFader (Section& s, const char* pid, juce::String name)
    {
        auto* f = new LabelledFader (proc.apvts, pid, std::move (name), proc.getMidiLearn());
        controls.add (f);
        s.addAndMakeVisible (f);
    }
    void addChoice (Section& s, const char* pid, juce::String name)
    {
        auto* c = new SegmentedControl (proc.apvts, pid, std::move (name), proc.getMidiLearn());
        controls.add (c);
        s.addAndMakeVisible (c);
    }

    void buildSections()
    {
        namespace ID = ParamID;
        const auto tOsc = VASynthLookAndFeel::accent();
        const auto tFilt = juce::Colour (0xff6ea8ff);
        const auto tEnv = juce::Colour (0xffb07cff);
        const auto tLfo = juce::Colour (0xfff0a04b);
        const auto tGlobal = juce::Colour (0xff8a929c);

        { auto& s = addSection ("Osc 1", tOsc, 1.7f);
          addChoice (s, ID::osc1Wave, "Wave"); addFader (s, ID::osc1Octave, "Oct");
          addFader (s, ID::osc1Detune, "Detune"); addFader (s, ID::osc1PW, "PW"); }

        { auto& s = addSection ("Osc 2", tOsc, 1.7f);
          addChoice (s, ID::osc2Wave, "Wave"); addFader (s, ID::osc2Octave, "Oct");
          addFader (s, ID::osc2Detune, "Detune"); addFader (s, ID::osc2PW, "PW"); }

        { auto& s = addSection ("Mix", tOsc, 0.8f);
          addFader (s, ID::oscMix, "Osc Mix"); addFader (s, ID::noiseLevel, "Noise"); }

        { auto& s = addSection ("Filter", tFilt, 1.7f);
          addChoice (s, ID::filterType, "Type"); addFader (s, ID::filterCutoff, "Cutoff");
          addFader (s, ID::filterReso, "Reso"); addFader (s, ID::filterEnvAmt, "Env");
          addFader (s, ID::filterKeytrack, "Track"); }

        { auto& s = addSection ("Amp Env", tEnv, 1.4f);
          addFader (s, ID::ampAttack, "A"); addFader (s, ID::ampDecay, "D");
          addFader (s, ID::ampSustain, "S"); addFader (s, ID::ampRelease, "R"); }

        { auto& s = addSection ("Filter Env", tEnv, 1.4f);
          addFader (s, ID::fltAttack, "A"); addFader (s, ID::fltDecay, "D");
          addFader (s, ID::fltSustain, "S"); addFader (s, ID::fltRelease, "R"); }

        { auto& s = addSection ("LFO", tLfo, 1.6f);
          addFader (s, ID::lfoRate, "Rate"); addFader (s, ID::lfoDepth, "Depth");
          addChoice (s, ID::lfoShape, "Shape"); addChoice (s, ID::lfoDest, "Dest"); }

        { auto& s = addSection ("Global", tGlobal, 1.8f);
          addFader (s, ID::glideTime, "Glide"); addFader (s, ID::masterGain, "Master");
          addChoice (s, ID::polyMode, "Mode");
          buildGlobalExtras (s); }
    }

    // Preset controls (Random / Save / Load) live in the Global section.
    void buildGlobalExtras (Section& s);

    void toggleFullscreen()
    {
        if (auto* w = getTopLevelComponent())
            if (auto* peer = w->getPeer())
                peer->setFullScreen (! peer->isFullScreen());
    }

    void timerCallback() override
    {
        const bool focused = hasKeyboardFocus (true);
        if (hadFocus && ! focused) allNotesOff();
        hadFocus = focused;
    }

    void emitNote (int note, bool on)
    {
        if (on) proc.qwertyKeyboardState.noteOn  (1, note, 0.8f);
        else    proc.qwertyKeyboardState.noteOff (1, note, 0.0f);
    }
    void allNotesOff() { qwerty.releaseAll ([this] (int note, bool on) { emitNote (note, on); }); }

    VASynthProcessor& proc;
    VASynthLookAndFeel lnf;
    std::vector<SectionEntry> sections;
    juce::OwnedArray<juce::Component> controls;
    DebugOverlay overlay { proc.health };
    PresetManager presets;
    QwertyKeyboard qwerty;
    bool hadFocus = false;

    // Preset UI (built in buildGlobalExtras).
    std::unique_ptr<juce::Component> presetPanel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VASynthEditor)
};
