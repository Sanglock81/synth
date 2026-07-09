#pragma once
#include "PluginProcessor.h"
#include "QwertyKeyboard.h"
#include <cstdlib>
#include <typeinfo>
#include "Observability/DebugOverlay.h"
#include "UI/VASynthLookAndFeel.h"
#include "UI/Widgets.h"
#include "UI/FXPanel.h"
#include "UI/ChordPanel.h"
#include "UI/InputsDialog.h"
#include "UI/PartsStrip.h"
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
                                      bool weHaveFocus, bool modalOpen, bool textFieldFocused,
                                      bool gestureActive)
{
    if (! standalone || ! showing || weHaveFocus) return false;
    if (! windowActive)                            return false;   // don't fight other apps
    if (modalOpen || textFieldFocused)             return false;   // don't steal typing
    if (gestureActive)                             return false;   // R2: never reclaim focus mid
                                                                   // touch/drag — grabbing focus
                                                                   // during a gesture makes the
                                                                   // FIRST touch on a control drop.
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

        // R2 touch diagnosis: env-gated global mouse-event trace. Run with
        // VASYNTH_TOUCH_TRACE=1 to record every touch/mouse event app-wide (finger index,
        // position, target, live-drag count, modal state) to the log — so a failed
        // first-touch on the Surface is visible and the root cause is data, not a guess.
        if (std::getenv ("VASYNTH_TOUCH_TRACE") != nullptr)
        {
            touchTrace = true;
            juce::Desktop::getInstance().addGlobalMouseListener (&tracer);
            juce::Logger::writeToLog ("TOUCH trace enabled");
        }
    }

    ~VASynthEditor() override
    {
        stopTimer();
        if (touchTrace) juce::Desktop::getInstance().removeGlobalMouseListener (&tracer);
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
        if (partsStrip != nullptr)                    // always-visible PARTS strip + INPUTS
        {
            partsStrip->setBounds (area.removeFromTop (30));
            area.removeFromTop (4);
        }
        juce::FlexBox row;
        row.flexDirection = juce::FlexBox::Direction::row;
        for (auto& s : sections)
            row.items.add (juce::FlexItem (*s.panel).withFlex (s.flex).withMargin (3.0f));
        if (chordPanel != nullptr)                    // CHORD column (before FX)
            row.items.add (juce::FlexItem (*chordPanel).withFlex (2.3f).withMargin (3.0f));
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

        // Chord modifiers on the reserved bottom row (7B): C=MAJ V=MIN B=7TH N=DOM7
        // M=SUS4 ,=SUS2 .=DIM /=spare. Published as a bitmask the processor diffs.
        const std::uint32_t mask = readChordModifierKeys();
        proc.setQwertyChordModifiers (mask);
        return qwerty.anyHeld() || mask != 0;
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

        // Three per-part LFOs (Sub-phase 2). Compact side-by-side sections; LFO 2/3
        // default depth 0 / dest Off (inert) so they don't change existing patches.
        { auto& s = addSection ("LFO 1", tLfo, 1.5f);
          addFader (s, ID::lfoRate, "Rate"); addFader (s, ID::lfoDepth, "Depth");
          addChoice (s, ID::lfoShape, "Shape"); addChoice (s, ID::lfoDest, "Dest"); }
        { auto& s = addSection ("LFO 2", tLfo, 1.5f);
          addFader (s, ID::lfo2Rate, "Rate"); addFader (s, ID::lfo2Depth, "Depth");
          addChoice (s, ID::lfo2Shape, "Shape"); addChoice (s, ID::lfo2Dest, "Dest"); }
        { auto& s = addSection ("LFO 3", tLfo, 1.5f);
          addFader (s, ID::lfo3Rate, "Rate"); addFader (s, ID::lfo3Depth, "Depth");
          addChoice (s, ID::lfo3Shape, "Shape"); addChoice (s, ID::lfo3Dest, "Dest"); }

        { auto& s = addSection ("Global", tGlobal, 2.5f);
          addFader (s, ID::glideTime, "Glide"); addFader (s, ID::velToAmp, "Vel>Amp");
          // MASTER is the headline output control: emphasised + given ~1.8x the
          // width of a normal fader so it's the obvious grab in the Global section.
          addFader (s, ID::masterGain, "Master", /*emphasise*/ true, /*flex*/ 1.8);
          addChoice (s, ID::polyMode, "Mode");
          buildGlobalExtras (s); }

        // MIX (Sub-phase 2): per-part level + pan. Level fixes the classic "kit too quiet
        // vs the lead" balance; pan spreads the parts. Defaults 1.0 / centre. MIDI-learnable.
        { auto& s = addSection ("Mix", tGlobal, 2.6f);
          addFader (s, ID::part0Level, "P0"); addFader (s, ID::part1Level, "P1");
          addFader (s, ID::part2Level, "P2"); addFader (s, ID::part3Level, "P3");
          addFader (s, ID::part0Pan, "Pan0"); addFader (s, ID::part1Pan, "Pan1");
          addFader (s, ID::part2Pan, "Pan2"); addFader (s, ID::part3Pan, "Pan3"); }

        // CHORD column (7B): enable/root/scale + momentary modifier indicators.
        chordPanel = std::make_unique<ChordPanel> (proc);
        addAndMakeVisible (*chordPanel);

        // Far-right reorderable FX column (its own draggable component, not a Section).
        fxPanel = std::make_unique<FXPanel> (proc);
        addAndMakeVisible (*fxPanel);

        // Always-visible PARTS strip + INPUTS button (discoverability for routing).
        partsStrip = std::make_unique<PartsStrip> (proc, [this] { restoreQwertyFocus(); });
        addAndMakeVisible (*partsStrip);
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
        // R2: but NEVER while a touch/drag gesture is live — grabbing focus mid-gesture
        // is a prime suspect for first-touch flakiness on the Surface.
        const bool gestureActive = juce::Desktop::getInstance().getNumDraggingMouseSources() > 0;
        if (qwertyShouldReclaimFocus (isStandalone(), isShowing(), topLevelWindowActive(),
                                      focused,
                                      juce::Component::getCurrentlyModalComponent() != nullptr,
                                      aTextFieldHasFocus(), gestureActive))
        {
            if (touchTrace) juce::Logger::writeToLog ("TOUCH focus-reclaim grab (drags=" + juce::String (juce::Desktop::getInstance().getNumDraggingMouseSources()) + ")");
            grabKeyboardFocus();
        }

        hadFocus = hasKeyboardFocus (true);

        // Surface any pending toast (MIDI hot-plug connect/disconnect).
        const int seq = proc.toastSequence();
        if (seq != lastToastSeq) { lastToastSeq = seq; toast.show (proc.toastMessage()); }
    }

    void emitNote (int note, bool on)
    {
        // QWERTY is a routed surface like any MIDI controller (Part B): its notes flow
        // through the "QWERTY" zone map, so it can be split and transposed too. Default
        // (no config) = the whole keyboard on the LIVE part.
        proc.routeSurfaceMessage ("QWERTY", on ? juce::MidiMessage::noteOn  (1, note, 0.8f)
                                                : juce::MidiMessage::noteOff (1, note));
    }
    void allNotesOff()
    {
        qwerty.releaseAll ([this] (int note, bool on) { emitNote (note, on); });
        proc.setQwertyChordModifiers (0);          // drop held modifiers on focus loss
    }

    // The reserved bottom row -> a modifier bitmask (bit = ChordEngine::ModifierId).
    static std::uint32_t readChordModifierKeys()
    {
        struct M { int key; int mod; };
        static constexpr M kMods[] {
            { 'c', ChordEngine::ModMaj  }, { 'v', ChordEngine::ModMin  },
            { 'b', ChordEngine::Mod7th  }, { 'n', ChordEngine::ModDom7 },
            { 'm', ChordEngine::ModSus4 }, { ',', ChordEngine::ModSus2 },
            { '.', ChordEngine::ModDim  }                              // '/' is spare
        };
        std::uint32_t mask = 0;
        for (auto& m : kMods)
            if (juce::KeyPress::isKeyCurrentlyDown (m.key)) mask |= (1u << m.mod);
        return mask;
    }

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

    // R2 touch diagnosis: global mouse-event tracer (env-gated; see constructor).
    struct TouchTracer : public juce::MouseListener
    {
        void emit (const char* kind, const juce::MouseEvent& e)
        {
            auto* c = e.eventComponent;
            juce::Logger::writeToLog (juce::String ("TOUCH ") + kind
                + " src="   + juce::String (e.source.getIndex()) + (e.source.isTouch() ? "t" : "m")
                + " @"      + e.getScreenPosition().toString()
                + " on="    + (c != nullptr ? (c->getName().isNotEmpty() ? c->getName() : juce::String (typeid (*c).name())) : juce::String ("null"))
                + " drags=" + juce::String (juce::Desktop::getInstance().getNumDraggingMouseSources())
                + " modal=" + juce::String (juce::Component::getCurrentlyModalComponent() != nullptr ? 1 : 0));
        }
        void mouseDown (const juce::MouseEvent& e) override { emit ("DOWN", e); }
        void mouseUp   (const juce::MouseEvent& e) override { emit ("UP  ", e); }
        void mouseDrag (const juce::MouseEvent& e) override { if ((++n & 7) == 0) emit ("DRAG", e); }
        int n = 0;
    };
    TouchTracer tracer;
    bool touchTrace = false;

    // Preset UI (built in buildGlobalExtras).
    std::unique_ptr<juce::Component> presetPanel;

    // Top PARTS strip, CHORD column, far-right reorderable FX column.
    std::unique_ptr<PartsStrip> partsStrip;
    std::unique_ptr<ChordPanel> chordPanel;
    std::unique_ptr<FXPanel> fxPanel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VASynthEditor)
};
