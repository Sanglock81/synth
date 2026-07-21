#pragma once
#include "PluginProcessor.h"
#include "ModDestRegistry.h"
#include "VersionInfo.h"          // build-fresh git hash for the on-screen banner
#include "QwertyKeyboard.h"
#include <cstdlib>
#include <typeinfo>
#include "Observability/DebugOverlay.h"
#include "UI/VASynthLookAndFeel.h"
#include "UI/Widgets.h"
#include "UI/Sections.h"
#include "UI/FXPanel.h"
#include "UI/TopBar.h"
#include "UI/PartRail.h"
#include "UI/ScopeView.h"
#include "UI/EQPanel.h"
#include "UI/BottomZones.h"
#include "UI/HelpOverlay.h"
#include "PresetManager.h"

// ============================================================================
// R2 hardware-style editor. One surface, everything visible: a top bar (preset /
// macros / master / CPU / help), a left PART RAIL, the centre synth in signal-flow
// order (OSC -> FILTER -> ENVELOPE -> LFO -> FX), a right SCOPE + FFT, and a bottom
// workstation (CHORD bar + collapsible RHYTHM / LOOPER). Controls bind to the APVTS
// via attachments; every control is MIDI-learnable (right-click / long-press) and
// refuses keyboard focus so QWERTY note input keeps working. Dark hardware LookAndFeel.
//
// Standalone: F11 fullscreen, F12 debug overlay, '?' help. Grab-mode touch controls.
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

// A banner shown while a drum-kit pad's voice is loaded into the main panel (Group 4 kit
// editing). DONE bakes the panel back into the pad and restores the previous focus.
class KitPadEditBar : public juce::Component
{
public:
    explicit KitPadEditBar (VASynthProcessor& p) : proc (p)
    {
        setWantsKeyboardFocus (false);
        done.setButtonText ("DONE"); done.setWantsKeyboardFocus (false);
        done.setColour (juce::TextButton::buttonColourId, VASynthLookAndFeel::accent());
        done.setColour (juce::TextButton::textColourOffId, juce::Colours::black);
        done.onClick = [this] { proc.endKitPadEdit (true); if (onDone) onDone(); };
        addAndMakeVisible (done);
    }
    void paint (juce::Graphics& g) override
    {
        g.setColour (VASynthLookAndFeel::accentWarm());
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);
        g.setColour (juce::Colours::black);
        g.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
        g.drawText ("EDITING KIT PAD " + juce::String (proc.editingKitPad() + 1)
                        + "  (P" + juce::String (proc.editingKitPart() + 1) + ")  -  tweak the panel, then",
                    getLocalBounds().withTrimmedLeft (14).withTrimmedRight (96), juce::Justification::centredLeft, false);
    }
    void resized() override { done.setBounds (getLocalBounds().removeFromRight (86).reduced (8, 6)); }
    std::function<void()> onDone;
private:
    VASynthProcessor& proc;
    juce::TextButton done;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KitPadEditBar)
};

// K2: when a KIT part is edit-focused, the synth-only panels (osc/filter/env/LFO) have no
// meaning — a drum kit's sound lives in its per-pad voices. This scrim dims that whole region
// and says so, with a one-tap route into the Kit Editor. The per-part EQ + mixer (elsewhere)
// stay live, so you still shape the kit's channel. Intercepts clicks so the dead controls
// beneath can't be fiddled by accident.
class KitScrim : public juce::Component
{
public:
    KitScrim()
    {
        setWantsKeyboardFocus (false);
        setInterceptsMouseClicks (true, true);
        openBtn.setButtonText ("Open Kit Editor");
        openBtn.setWantsKeyboardFocus (false);
        openBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff67c0c8));
        openBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::black);
        openBtn.onClick = [this] { if (onOpen) onOpen(); };
        addAndMakeVisible (openBtn);
    }
    std::function<void()> onOpen;
    void setPartLabel (const juce::String& s) { if (s != partLabel) { partLabel = s; repaint(); } }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xea0e1319));                      // heavy dim scrim
        g.fillRoundedRectangle (r, 8.0f);
        g.setColour (juce::Colour (0xff67c0c8).withAlpha (0.55f));
        g.drawRoundedRectangle (r.reduced (1.5f), 8.0f, 1.5f);

        auto body = getLocalBounds().reduced (18);
        g.setColour (juce::Colour (0xff67c0c8));
        g.setFont (juce::Font (juce::FontOptions (17.0f, juce::Font::bold)));
        g.drawFittedText ("KIT  -  " + partLabel, body.removeFromTop (30), juce::Justification::centred, 1);
        g.setColour (VASynthLookAndFeel::dim());
        g.setFont (juce::Font (juce::FontOptions (12.5f)));
        g.drawFittedText ("These synth controls don't apply to a drum kit.\n"
                          "Edit each pad's sound in the Kit Editor.\n"
                          "The EQ (right) still shapes the whole kit.",
                          body.removeFromTop (70), juce::Justification::centredTop, 3);
    }
    void resized() override
    { openBtn.setBounds (getLocalBounds().withSizeKeepingCentre (176, 34).withY (getHeight() * 62 / 100)); }

private:
    juce::String partLabel;
    juce::TextButton openBtn;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KitScrim)
};

class VASynthEditor : public juce::AudioProcessorEditor,
                      private juce::Timer
{
public:
    static constexpr int kDefaultWidth = 1760;

    explicit VASynthEditor (VASynthProcessor& p)
        : AudioProcessorEditor (p), proc (p), presets (p.apvts)
    {
        setLookAndFeel (&lnf);

        buildUI();
        wireModTargets (*this);   // ONE place: every registry-bound control becomes a LINK target + animates
        // G6 intake trace: F12 overlay shows the live pitch-bend / mod-wheel value and a running
        // count of how many of each has arrived — so a dead strip is instantly diagnosable
        // (0 events => the controller isn't sending; events but no sound => downstream).
        overlay.setExtraLinesProvider ([this]
        {
            juce::StringArray lines {
                "bend  " + juce::String (proc.currentPitchBendSemis(), 2) + " st   (" + juce::String ((int) proc.pitchBendEventCount()) + " ev)",
                "mod   " + juce::String (proc.currentModWheel(), 2)       + "      (" + juce::String ((int) proc.modWheelEventCount())  + " ev)" };
            // Live MIDI monitor: the last few incoming messages with their surface / channel / note,
            // so pad-vs-key channel splits (and any device) are diagnosable at a glance.
            lines.add ("-- MIDI in (newest first) --");
            lines.addArray (proc.midiMonitorLines());
            return lines;
        });
        // Version banner (H0): the running build, confirmable on screen. VASYNTH_GIT_HASH_RT is
        // regenerated every build so it always names the exact binary. Shown persistently in the
        // F12 overlay and flashed as a toast at launch (the log-only banner was invisible before).
        versionBanner = juce::String ("synth ") + VASYNTH_VERSION + "   git " + VASYNTH_GIT_HASH_RT
                        + "   " + VASYNTH_BUILD_TYPE;
        overlay.setVersionLine (versionBanner);
        if (isStandalone()) proc.postToast (versionBanner);

        addChildComponent (overlay);
        addChildComponent (toast);
        addChildComponent (helpOverlay);
        helpOverlay.onDismiss = [this] { hideHelp(); };
        lastToastSeq = proc.toastSequence();       // don't replay a pre-existing toast on open

        setResizable (true, true);
        setResizeLimits (900, 480, 3200, 1800);

        int w = kDefaultWidth, h = 1000;
        if (auto* disp = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        {
            const auto area = disp->userArea;
            w = juce::jmin (w, area.getWidth()  - 40);
            h = juce::jmin (h, area.getHeight() - 40);
        }
        setSize (w, h);

        if (isStandalone()) setWantsKeyboardFocus (true);
        startTimerHz (30);                // focus-loss watchdog (standalone) + kit-pad-edit banner poll

        addChildComponent (kitBar);
        kitBar.onDone = [this] { refreshAfterKitPadEdit(); };

        // R2 touch diagnosis: env-gated global mouse-event trace (VASYNTH_TOUCH_TRACE=1).
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

    void paint (juce::Graphics& g) override { g.fillAll (VASynthLookAndFeel::panel().darker (0.25f)); }

    // Clicking the panel background returns keyboard focus to the editor so
    // QWERTY resumes (controls refuse focus, so this only fires on the backdrop).
    void mouseDown (const juce::MouseEvent&) override { grabQwertyFocus(); }

    void parentHierarchyChanged() override { grabQwertyFocus(); }
    void visibilityChanged() override      { grabQwertyFocus(); }

    // Restores QWERTY focus after a transient dialog (e.g. Save-preset) closes.
    void restoreQwertyFocus() { grabQwertyFocus(); }

    // After a kit pad edit commits, hide the banner and repaint (the panel's attachments
    // auto-reflect the restored focus params).
    void refreshAfterKitPadEdit() { kitBar.setVisible (false); repaint(); restoreQwertyFocus(); }

    void resized() override
    {
        // Tall + wide enough for the core health lines PLUS the bend/mod trace and the full
        // MIDI monitor (long surface names). Previously 78 px -> every extra line was clipped.
        overlay.setBounds (getWidth() - 452, 92, 444, 360);
        toast.setBounds ((getWidth() - 460) / 2, 92, 460, 46);
        kitBar.setBounds ((getWidth() - 620) / 2, 96, 620, 40);
        helpOverlay.setBounds (getLocalBounds());

        auto area = getLocalBounds().reduced (6);
        const int gap = 5;

        topBar->setBounds (area.removeFromTop (86)); area.removeFromTop (gap);

        if (bottomZones != nullptr)
        {
            const int bh = juce::jmin (area.getHeight() - 120, bottomZones->preferredHeight());
            bottomZones->setBounds (area.removeFromBottom (juce::jmax (60, bh)));
            area.removeFromBottom (gap);
        }

        partRail->setBounds (area.removeFromLeft (232)); area.removeFromLeft (gap);

        auto right = area.removeFromRight (286); area.removeFromRight (gap);
        scopeView->setBounds (right.removeFromTop (right.getHeight() * 40 / 100)); right.removeFromTop (gap);   // scope+spectrum stack; EQ gets the larger share (B2)
        eqPanel->setBounds (right);

        auto centre = area;
        const int u = juce::jmax (70, (centre.getWidth() - 4 * gap) / 13);
        auto placeL = [&] (juce::Component& c, int units)
        {
            c.setBounds (centre.removeFromLeft (juce::jmin (u * units, centre.getWidth())));
            centre.removeFromLeft (gap);
        };
        placeL (*oscSection, 3);
        placeL (*filterSection, 2);
        placeL (*envSection, 2);
        placeL (*lfoSection, 3);
        fxPanel->setBounds (centre);

        // The kit scrim covers the whole synth-panel region (osc..lfo).
        kitScrim.setBounds (oscSection->getBounds().getUnion (lfoSection->getBounds()));
        refreshKitScrim();
    }

    // F11 fullscreen (standalone), F12 debug overlay, '?' help.
    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::F12Key) { overlay.setVisible (! overlay.isVisible()); return true; }
        if (key == juce::KeyPress::F11Key && isStandalone()) { toggleFullscreen(); return true; }
        if (key.getTextCharacter() == '?') { toggleHelp(); return true; }
        if (key == juce::KeyPress::escapeKey && helpOverlay.isVisible()) { hideHelp(); return true; }
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
    bool isStandalone() const { return proc.wrapperType == juce::AudioProcessor::wrapperType_Standalone; }

    // The SINGLE mod-target path: walk the whole control tree and, for every parameter-attached
    // control whose parameter is in the destination registry, attach LINK + animation. No control
    // is ever hand-wired for modulation; a new parameter joins simply by being in the registry.
    void wireModTargets (juce::Component& c)
    {
        for (auto* child : c.getChildren())
        {
            if (auto* lc = dynamic_cast<LearnableComponent*> (child))
            {
                const int dest = moddest::destForParam (lc->parameterID());
                if (dest != ModMatrix::DstNone)
                {
                    auto* param = proc.apvts.getParameter (lc->parameterID());
                    lc->setModTarget (proc, dest, [this, dest, param] { return proc.modAnimNorm (dest, param); });
                }
            }
            wireModTargets (*child);
        }
    }

    void buildUI()
    {
        topBar = std::make_unique<TopBar> (proc, presets,
                                           [this] { restoreQwertyFocus(); },
                                           [this] { toggleHelp(); },
                                           [this] { toggleFullscreen(); });
        addAndMakeVisible (*topBar);

        partRail = std::make_unique<PartRail> (proc, [this] { restoreQwertyFocus(); });
        addAndMakeVisible (*partRail);

        oscSection    = std::make_unique<OscSection> (proc);    addAndMakeVisible (*oscSection);
        filterSection = std::make_unique<FilterSection> (proc); addAndMakeVisible (*filterSection);
        envSection    = std::make_unique<EnvSection> (proc);    addAndMakeVisible (*envSection);
        lfoSection    = std::make_unique<LfoSection> (proc);    addAndMakeVisible (*lfoSection);

        fxPanel   = std::make_unique<FXPanel> (proc);   addAndMakeVisible (*fxPanel);
        scopeView = std::make_unique<ScopeView> (proc); addAndMakeVisible (*scopeView);
        eqPanel   = std::make_unique<EQPanel> (proc);   addAndMakeVisible (*eqPanel);

        bottomZones = std::make_unique<BottomZones> (proc);
        bottomZones->onResizeNeeded = [this] { resized(); };
        addAndMakeVisible (*bottomZones);

        addChildComponent (kitScrim);       // shown only while a kit part is edit-focused (K2)
        kitScrim.onOpen = [this]
        {
            KitEditor::show (proc, getTopLevelComponent(), proc.editFocus(),
                             [this] { restoreQwertyFocus(); },
                             [this] (int pt, int pd) { proc.beginKitPadEdit (pt, pd); });
        };
    }

    void grabQwertyFocus()
    {
        if (! isStandalone() || ! isShowing()) return;
        grabKeyboardFocus();
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

    void toggleHelp()
    {
        const bool show = ! helpOverlay.isVisible();
        helpOverlay.setVisible (show);
        if (show) helpOverlay.toFront (false);
        else      grabQwertyFocus();
    }
    void hideHelp() { helpOverlay.setVisible (false); grabQwertyFocus(); }

    // True full-screen via JUCE kiosk mode: the window fills the display and sits on
    // top, so you can't accidentally click an app behind it. Toggles back to windowed.
    // Standalone only (a plugin must never take over the host's screen).
    void toggleFullscreen()
    {
        if (! isStandalone()) return;
        auto& desktop = juce::Desktop::getInstance();
        if (desktop.getKioskModeComponent() != nullptr)
            desktop.setKioskModeComponent (nullptr);
        else if (auto* top = getTopLevelComponent())
            desktop.setKioskModeComponent (top, false);
        grabQwertyFocus();
    }

    void timerCallback() override
    {
        const bool focused = hasKeyboardFocus (true);
        if (hadFocus && ! focused) allNotesOff();

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

        const int seq = proc.toastSequence();
        if (seq != lastToastSeq) { lastToastSeq = seq; toast.show (proc.toastMessage()); }

        // Kit pad-edit banner: shown while a pad's voice is loaded into the main panel.
        const bool editing = proc.isEditingKitPad();
        if (editing != kitBar.isVisible()) { kitBar.setVisible (editing); if (editing) { kitBar.toFront (false); kitBar.repaint(); } }

        refreshKitScrim();
    }

    // K2: dim the synth panels when the edit-focused part is a drum kit (its EQ still edits).
    void refreshKitScrim()
    {
        const int f = proc.editFocus();
        const bool kitFocused = proc.isPartKit (f) && ! proc.isEditingKitPad();   // pad-edit temporarily makes it a synth
        if (kitFocused != kitScrim.isVisible())
        { kitScrim.setVisible (kitFocused); if (kitFocused) kitScrim.toFront (false); }
        if (kitFocused) kitScrim.setPartLabel ("P" + juce::String (f + 1) + "   " + proc.getPartPreset (f));
    }

    void emitNote (int note, bool on)
    {
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
        // Physical key order (c v b n m , .) maps left-to-right onto the CHORD bar's chip
        // order (MAJ MIN SUS4 SUS2 DIM DOM7 7TH) so the key you press lights the chip above it.
        struct M { int key; int mod; };
        static constexpr M kMods[] {
            { 'c', ChordEngine::ModMaj  }, { 'v', ChordEngine::ModMin  },
            { 'b', ChordEngine::ModSus4 }, { 'n', ChordEngine::ModSus2 },
            { 'm', ChordEngine::ModDim  }, { ',', ChordEngine::ModDom7 },
            { '.', ChordEngine::Mod7th  }                              // '/' is spare
        };
        std::uint32_t mask = 0;
        for (auto& m : kMods)
            if (juce::KeyPress::isKeyCurrentlyDown (m.key)) mask |= (1u << m.mod);
        return mask;
    }

    VASynthProcessor& proc;
    VASynthLookAndFeel lnf;
    // Hover help: after 1 s over any control, show its full parameter name (controls set their
    // own tooltip from the APVTS name). One window services the whole editor.
    juce::TooltipWindow tooltips { this, 1000 };
    DebugOverlay overlay { proc.health };
    juce::String versionBanner;
    PresetManager presets;
    QwertyKeyboard qwerty;
    Toast toast;
    HelpOverlay helpOverlay;
    KitPadEditBar kitBar { proc };
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

    // R2 layout components.
    std::unique_ptr<TopBar> topBar;
    std::unique_ptr<PartRail> partRail;
    std::unique_ptr<OscSection> oscSection;
    std::unique_ptr<FilterSection> filterSection;
    std::unique_ptr<EnvSection> envSection;
    std::unique_ptr<LfoSection> lfoSection;
    std::unique_ptr<FXPanel> fxPanel;
    std::unique_ptr<ScopeView> scopeView;
    std::unique_ptr<EQPanel> eqPanel;
    std::unique_ptr<BottomZones> bottomZones;
    KitScrim kitScrim;                     // K2: dims osc/filter/env/LFO while a kit is focused

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VASynthEditor)
};
