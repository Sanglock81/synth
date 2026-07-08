#pragma once
#include "PluginProcessor.h"
#include "QwertyKeyboard.h"
#include "Observability/DebugOverlay.h"
#include "UI/VASynthLookAndFeel.h"
#include "UI/Widgets.h"
#include "UI/FXPanel.h"
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

// Bug B: should the standalone editor RECLAIM keyboard focus for QWERTY play?
// QWERTY input dies when a transient thief takes focus and never returns it — the
// Load combo, the Audio/MIDI settings dialog, or the window being re-activated
// after Alt-Tab. The 30 Hz watchdog reclaims focus whenever this predicate is true:
//   standalone + on screen + our window active + we DON'T already hold focus —
//   and NO modal dialog or text field is up (else we'd steal Save/INPUTS typing).
// Pure + free-standing so every case is unit-tested without a live window.
inline bool qwertyShouldReclaimFocus (bool standalone, bool showing, bool windowActive,
                                      bool weHaveFocus, bool modalOpen, bool textFieldFocused)
{
    if (! standalone || ! showing || weHaveFocus) return false;
    if (! windowActive)                            return false;   // don't fight other apps
    if (modalOpen || textFieldFocused)             return false;   // don't steal typing
    return true;
}

class VASynthEditor : public juce::AudioProcessorEditor,
                      private juce::Timer
{
public:
    // Default window width — sized so 56 px fader thumbs are met at default
    // scale for the full control count (grows as sections are added).
    static constexpr int kDefaultWidth = 2760;

    explicit VASynthEditor (VASynthProcessor& p)
        : AudioProcessorEditor (p), proc (p), presets (p.apvts)
    {
        setLookAndFeel (&lnf);

        buildSections();
        addChildComponent (overlay);
        addChildComponent (toast);
        lastToastSeq = proc.toastSequence();       // don't replay a pre-existing toast on open

        setResizable (true, true);
        setResizeLimits (900, 480, 3000, 1600);

        // Default startup size. Wide enough that the 56 px fader thumbs are hit
        // at default scale; clamped to the display so it never opens off-screen
        // (on a narrower screen the FlexBox scales down proportionally).
        int w = kDefaultWidth, h = 640;
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
        overlay.setBounds (getWidth() - 320, 8, 312, 78);   // 4 lines incl. SAT
        toast.setBounds ((getWidth() - 460) / 2, 14, 460, 46);   // top-centre

        auto area = getLocalBounds().reduced (6);
        juce::FlexBox row;
        row.flexDirection = juce::FlexBox::Direction::row;
        for (auto& s : sections)
            row.items.add (juce::FlexItem (*s.panel).withFlex (s.flex).withMargin (3.0f));
        if (fxPanel != nullptr)                       // far-right FX column
            row.items.add (juce::FlexItem (*fxPanel).withFlex (2.8f).withMargin (3.0f));
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

    bool topLevelWindowActive() const
    {
        if (auto* tlw = dynamic_cast<juce::TopLevelWindow*> (getTopLevelComponent()))
            return tlw->isActiveWindow();
        return isShowing();
    }
    static bool aTextFieldHasFocus()
    {
        auto* f = juce::Component::getCurrentlyFocusedComponent();
        return dynamic_cast<juce::TextEditor*> (f) != nullptr;
    }

    Section& addSection (juce::String title, juce::Colour tint, float flex)
    {
        auto s = std::make_unique<Section> (std::move (title), tint);
        addAndMakeVisible (*s);
        auto& ref = *s;
        sections.push_back ({ std::move (s), flex });
        return ref;
    }

    void addFader (Section& s, const char* pid, juce::String name,
                   bool emphasise = false, double flex = 1.0)
    {
        auto* f = new LabelledFader (proc.apvts, pid, std::move (name), proc.getMidiLearn(), emphasise);
        f->getProperties().set ("layoutFlex", flex);
        controls.add (f);
        s.addAndMakeVisible (f);
    }
    void addChoice (Section& s, const char* pid, juce::String name)
    {
        auto* c = new SegmentedControl (proc.apvts, pid, std::move (name), proc.getMidiLearn());
        controls.add (c);
        s.addAndMakeVisible (c);
    }
    void addToggle (Section& s, const char* pid, juce::String name)
    {
        auto* t = new PowerToggle (proc.apvts, pid, std::move (name));
        controls.add (t);
        s.addAndMakeVisible (t);
    }

    void buildSections()
    {
        namespace ID = ParamID;
        const auto tOsc = VASynthLookAndFeel::accent();
        const auto tFilt = juce::Colour (0xff6ea8ff);
        const auto tEnv = juce::Colour (0xffb07cff);
        const auto tLfo = juce::Colour (0xfff0a04b);
        const auto tGlobal = juce::Colour (0xff8a929c);

        { auto& s = addSection ("Osc 1", tOsc, 1.9f);
          addToggle (s, ID::osc1On, "ON"); addChoice (s, ID::osc1Wave, "Wave"); addFader (s, ID::osc1Octave, "Oct");
          addFader (s, ID::osc1Detune, "Detune"); addFader (s, ID::osc1PW, "PW"); }

        { auto& s = addSection ("Osc 2", tOsc, 1.9f);
          addToggle (s, ID::osc2On, "ON"); addChoice (s, ID::osc2Wave, "Wave"); addFader (s, ID::osc2Octave, "Oct");
          addFader (s, ID::osc2Detune, "Detune"); addFader (s, ID::osc2PW, "PW"); }

        { auto& s = addSection ("Osc 3", tOsc, 1.9f);
          addToggle (s, ID::osc3On, "ON"); addChoice (s, ID::osc3Wave, "Wave"); addFader (s, ID::osc3Octave, "Oct");
          addFader (s, ID::osc3Detune, "Detune"); addFader (s, ID::osc3PW, "PW"); }

        { auto& s = addSection ("Mix", tOsc, 1.4f);
          addFader (s, ID::osc1Level, "Osc1"); addFader (s, ID::osc2Level, "Osc2");
          addFader (s, ID::osc3Level, "Osc3"); addFader (s, ID::noiseLevel, "Noise"); }

        { auto& s = addSection ("Filter", tFilt, 2.2f);
          addChoice (s, ID::filterType, "Type"); addFader (s, ID::filterCutoff, "Cutoff");
          addFader (s, ID::filterReso, "Reso"); addFader (s, ID::filterEnvAmt, "Env");
          addFader (s, ID::filterKeytrack, "Track"); addFader (s, ID::velToCutoff, "Vel>Cut"); }

        { auto& s = addSection ("Amp Env", tEnv, 1.4f);
          addFader (s, ID::ampAttack, "A"); addFader (s, ID::ampDecay, "D");
          addFader (s, ID::ampSustain, "S"); addFader (s, ID::ampRelease, "R"); }

        { auto& s = addSection ("Mod Env", tEnv, 1.7f);   // filter env, now also -> pitch
          addFader (s, ID::fltAttack, "A"); addFader (s, ID::fltDecay, "D");
          addFader (s, ID::fltSustain, "S"); addFader (s, ID::fltRelease, "R");
          addFader (s, ID::fltEnvToPitch, "Pitch"); }

        { auto& s = addSection ("LFO", tLfo, 1.6f);
          addFader (s, ID::lfoRate, "Rate"); addFader (s, ID::lfoDepth, "Depth");
          addChoice (s, ID::lfoShape, "Shape"); addChoice (s, ID::lfoDest, "Dest"); }

        { auto& s = addSection ("Global", tGlobal, 2.5f);
          addFader (s, ID::glideTime, "Glide"); addFader (s, ID::velToAmp, "Vel>Amp");
          // MASTER is the headline output control: emphasised + given ~1.8x the
          // width of a normal fader so it's the obvious grab in the Global section.
          addFader (s, ID::masterGain, "Master", /*emphasise*/ true, /*flex*/ 1.8);
          addChoice (s, ID::polyMode, "Mode");
          buildGlobalExtras (s); }

        // Far-right reorderable FX column (its own draggable component, not a Section).
        fxPanel = std::make_unique<FXPanel> (proc);
        addAndMakeVisible (*fxPanel);
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

        // Bug B: reclaim keyboard focus for QWERTY play after a transient thief
        // (Load combo / settings dialog close / window re-activation) let it go.
        if (qwertyShouldReclaimFocus (isStandalone(), isShowing(), topLevelWindowActive(),
                                      focused,
                                      juce::Component::getCurrentlyModalComponent() != nullptr,
                                      aTextFieldHasFocus()))
            grabKeyboardFocus();

        hadFocus = hasKeyboardFocus (true);

        // Surface any pending toast (MIDI hot-plug connect/disconnect).
        const int seq = proc.toastSequence();
        if (seq != lastToastSeq) { lastToastSeq = seq; toast.show (proc.toastMessage()); }
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
    Toast toast;
    int  lastToastSeq = 0;
    bool hadFocus = false;

    // Preset UI (built in buildGlobalExtras).
    std::unique_ptr<juce::Component> presetPanel;

    // Far-right reorderable FX column.
    std::unique_ptr<FXPanel> fxPanel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VASynthEditor)
};
