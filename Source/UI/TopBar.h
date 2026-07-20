#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "VASynthLookAndFeel.h"
#include "Widgets.h"
#include "ModMatrixPanel.h"
#include "InputsDialog.h"
#include "OutputsDialog.h"
#include "../PluginProcessor.h"
#include "../PresetManager.h"

// ============================================================================
// R2 top bar: the preset name (tap to load), Save / Random, a live CPU readout,
// eight MACRO knobs (M1-M8), the big MASTER knob, a REC placeholder (the looper
// lands in R3), and the help (?) button. Refuses keyboard focus so QWERTY note
// input keeps working; the preset menu restores focus on close.
// ============================================================================

class TopBar : public juce::Component,
               private juce::Timer
{
public:
    TopBar (VASynthProcessor& p, PresetManager& pm,
            std::function<void()> restoreFocusFn, std::function<void()> toggleHelpFn,
            std::function<void()> toggleFullscreenFn)
        : proc (p), presets (pm),
          restoreFocus (std::move (restoreFocusFn)), toggleHelp (std::move (toggleHelpFn)),
          toggleFullscreen (std::move (toggleFullscreenFn))
    {
        setWantsKeyboardFocus (false);

        auto styleBtn = [] (juce::TextButton& b)
        {
            b.setWantsKeyboardFocus (false);
            b.setColour (juce::TextButton::buttonColourId, VASynthLookAndFeel::track());
            b.setColour (juce::TextButton::textColourOffId, VASynthLookAndFeel::ink());
        };

        presetBtn.setButtonText ("synth  -  Init");
        styleBtn (presetBtn);
        presetBtn.onClick = [this] { showPresetMenu(); };
        addAndMakeVisible (presetBtn);

        save.setButtonText ("SAVE"); styleBtn (save);
        save.onClick = [this] { showSaveDialog(); };
        addAndMakeVisible (save);

        random.setButtonText ("RANDOM"); styleBtn (random);
        // Left-click / tap = roll a mode (NEW). Right-click or long-press = pick a mode explicitly.
        random.onClick = [this] { if (suppressRandClick) { suppressRandClick = false; return; } rollRandom(); };
        random.addMouseListener (this, false);
        addAndMakeVisible (random);

        vary.setButtonText ("VARY"); styleBtn (vary);
        vary.onClick = [this]
        {
            juce::Random rng;
            proc.varySound (rng);                 // nudge the current focused-part patch a little
            currentName = "Varied"; refreshTitle(); refreshMacroLabels();
            proc.postToast ("VARY"); if (auto* t = getTopLevelComponent()) t->repaint();
            if (restoreFocus) restoreFocus();
        };
        addAndMakeVisible (vary);

        clear.setButtonText ("CLEAR"); styleBtn (clear);
        clear.onClick = [this]
        {
            proc.clearFocusedPartToBlank();     // blank the selected part to a clean sine
            currentName = "Blank"; refreshTitle(); refreshMacroLabels();
            if (restoreFocus) restoreFocus();
        };
        addAndMakeVisible (clear);

        rec.setButtonText ("REC"); styleBtn (rec);
        rec.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffd8443a));
        rec.onClick = [this] { proc.postToast ("Recording lands with the looper (R3)"); if (restoreFocus) restoreFocus(); };
        addAndMakeVisible (rec);

        full.setButtonText ("FS"); styleBtn (full);
        full.onClick = [this] { if (toggleFullscreen) toggleFullscreen(); if (restoreFocus) restoreFocus(); };
        addAndMakeVisible (full);

        help.setButtonText ("?"); styleBtn (help);
        help.onClick = [this] { if (toggleHelp) toggleHelp(); };
        addAndMakeVisible (help);

        // Global-action row (below SAVE/RANDOM/CLEAR, beside the CPU readout): LINK arms a mod
        // source, MOD opens the routing overlay, INPUTS opens the surface-routing dialog. These
        // are global actions, not part-rail furniture (moved here from the Parts rail header).
        link.setButtonText ("LINK"); styleBtn (link);
        link.setColour (juce::TextButton::textColourOffId, kLinkRing);
        link.onClick = [this] { onLinkClicked(); };
        addAndMakeVisible (link);

        mod.setButtonText ("MOD"); styleBtn (mod);
        mod.setColour (juce::TextButton::textColourOffId, VASynthLookAndFeel::accent());
        mod.onClick = [this] { ModMatrixPanel::show (proc, getTopLevelComponent(), [this] { if (restoreFocus) restoreFocus(); }); };
        addAndMakeVisible (mod);

        inputs.setButtonText ("INPUTS"); styleBtn (inputs);
        inputs.onClick = [this] { InputsDialog::show (proc, getTopLevelComponent(), [this] { if (restoreFocus) restoreFocus(); }); };
        addAndMakeVisible (inputs);

        outputs.setButtonText ("OUTPUTS"); styleBtn (outputs);
        outputs.onClick = [this] { OutputsDialog::show (proc, getTopLevelComponent(), [this] { if (restoreFocus) restoreFocus(); }); };
        addAndMakeVisible (outputs);

        // The 8 macros live in the top bar (compact, with the preset + master). The
        // native title-bar gesture issue is handled by running fullscreen/kiosk (no title
        // bar) — see toggleFullscreen / the README live-mode note.
        const char* ids[] { ParamID::macro1, ParamID::macro2, ParamID::macro3, ParamID::macro4,
                            ParamID::macro5, ParamID::macro6, ParamID::macro7, ParamID::macro8 };
        for (int m = 0; m < 8; ++m)
        {
            auto* k = new RotaryKnob (proc.apvts, ids[m], "M" + juce::String (m + 1), proc.getMidiLearn());
            k->setShowValue (false);                             // no number -> compact
            k->setDragPixels ((kDragPixelsForFullRange * 10) / 27);   // ~2.7x responsive
            k->setBothAxisDrag();   // top-of-window: allow horizontal drag so touch needn't go up into the title bar
            macros.add (k); addAndMakeVisible (k);
            const int idx = m;
            macroAtt.add (new juce::ParameterAttachment (*proc.apvts.getParameter (ids[m]),
                [this, idx] (float v) { applyMacro (idx, v); }, nullptr));
            // Macro context menu (long-press / right-click): restore the factory default, rename.
            k->addContextMenuItem ("Restore default assignment",
                [this, idx] { proc.restoreMacroDefault (idx); refreshMacroLabels(); if (restoreFocus) restoreFocus(); });
            k->addContextMenuItem ("Rename macro...", [this, idx] { renameMacro (idx); });
        }

        master = std::make_unique<RotaryKnob> (proc.apvts, ParamID::masterGain, "MASTER", proc.getMidiLearn());
        addAndMakeVisible (*master);

        // Voice controls (ex-Global): poly/mono/legato + glide.
        mode  = std::make_unique<HSelector> (proc.apvts, ParamID::polyMode, proc.getMidiLearn(),
                                             juce::StringArray { "POLY", "MONO", "LEG" });
        glide = std::make_unique<RotaryKnob> (proc.apvts, ParamID::glideTime, "GLIDE", proc.getMidiLearn());
        analog = std::make_unique<RotaryKnob> (proc.apvts, ParamID::analog, "ANALOG", proc.getMidiLearn());   // Tier 1b drift
        addAndMakeVisible (*mode); addAndMakeVisible (*glide); addAndMakeVisible (*analog);

        refreshMacroLabels();
        startTimerHz (4);   // CPU readout + macro-label resync
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (VASynthLookAndFeel::panelLight());
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);

        g.setColour (VASynthLookAndFeel::dim());
        g.setFont (juce::Font (juce::FontOptions (11.0f)));
        g.drawText (statusLine, statusArea, juce::Justification::centredLeft, false);
    }

    void resized() override
    {
        auto tb = getLocalBounds().reduced (10, 7);

        auto preset = tb.removeFromLeft (300); tb.removeFromLeft (30);   // J4#2: bigger gap before the voice group
        auto pTop = preset.removeFromTop (34);
        presetBtn.setBounds (pTop.removeFromLeft (126).reduced (0, 1));
        save.setBounds   (pTop.removeFromLeft (44).reduced (2, 1));
        random.setBounds (pTop.removeFromLeft (66).reduced (2, 1));
        clear.setBounds  (pTop.reduced (2, 1));
        preset.removeFromTop (8);                                        // J4#3: nudge the action row down
        // Global-action row below the preset row: LINK | MOD | INPUTS on the right, VARY + CPU left.
        auto grow = preset.removeFromTop (26);
        inputs.setBounds  (grow.removeFromRight (62).reduced (2, 1));
        outputs.setBounds (grow.removeFromRight (70).reduced (2, 1));
        mod.setBounds     (grow.removeFromRight (46).reduced (2, 1));
        link.setBounds    (grow.removeFromRight (46).reduced (2, 1));
        vary.setBounds    (grow.removeFromLeft (44).reduced (2, 1));
        statusArea = grow;

        auto help_ = tb.removeFromRight (34); tb.removeFromRight (4);
        help.setBounds (help_.reduced (0, 16));
        full.setBounds (tb.removeFromRight (36).reduced (0, 16)); tb.removeFromRight (6);
        master->setBounds (tb.removeFromRight (92)); tb.removeFromRight (4);
        rec.setBounds (tb.removeFromRight (54).reduced (0, 16)); tb.removeFromRight (10);

        // Voice group (poly/mono/legato + glide), just right of the preset area.
        mode->setBounds (tb.removeFromLeft (120).withSizeKeepingCentre (120, 30)); tb.removeFromLeft (6);
        glide->setBounds  (tb.removeFromLeft (52)); tb.removeFromLeft (4);
        analog->setBounds (tb.removeFromLeft (52)); tb.removeFromLeft (10);

        // 8 macro knobs, packed to 75% of the middle span (J4#1: closer together), centred.
        const int n = macros.size();
        const int span = tb.getWidth();
        const int mw = juce::jmax (10, (span * 3 / 4) / n);
        tb.removeFromLeft (juce::jmax (0, (span - mw * n) / 2));
        for (int m = 0; m < n; ++m)
            macros[m]->setBounds (tb.removeFromLeft (mw).reduced (2, 0));
    }

    // Update the shown patch name from outside (e.g. a factory load elsewhere).
    void setCurrentName (juce::String n) { currentName = std::move (n); refreshTitle(); }

private:
    void timerCallback() override
    {
        const int cpu = (int) juce::roundToInt (proc.health.snapshot().cpuPercent);
        auto line = "CPU " + juce::String (juce::jlimit (0, 999, cpu)) + "%";
        if (line != statusLine) { statusLine = line; repaint (statusArea); }
        refreshMacroLabels();     // map can change on Random / preset load

        // A completed knob-tap auto-disarms the source; keep the LINK button label in sync.
        if ((proc.modLinkArmedSource() >= 0) != linkWasArmed)
        { linkWasArmed = proc.modLinkArmedSource() >= 0; refreshLinkButton(); repaintTop(); }

        // RANDOM long-press -> the explicit mode picker (touch-friendly; suppresses the release-click).
        if (randArmed && juce::Time::getMillisecondCounter() - randDownMs > 500)
        { randArmed = false; suppressRandClick = true; showRandomModeMenu(); }

        // Poly/Mono/Legato + glide are per-part VOICE controls; a kit part is always poly
        // and its pads don't glide, so grey them out (disabled) when a kit is the active part.
        const bool kitActive = proc.isPartKit (proc.playFocus());
        if (kitActive != voiceCtrlsDisabled)
        {
            voiceCtrlsDisabled = kitActive;
            if (mode)  { mode->setEnabled  (! kitActive); mode->setAlpha  (kitActive ? 0.35f : 1.0f); }
            if (glide) { glide->setEnabled (! kitActive); glide->setAlpha (kitActive ? 0.35f : 1.0f); }
        }
    }

    // LINK: armed? disarm. Otherwise pop the source list; picking one arms it and lights every
    // destination knob's ring (a top-level repaint) so the next knob tap makes the route.
    void onLinkClicked()
    {
        if (proc.modLinkArmedSource() >= 0) { proc.disarmModLink(); refreshLinkButton(); repaintTop(); return; }
        const auto names = ModMatrixPanel::sourceNames();     // index 0 = "None" (skip)
        juce::PopupMenu m;
        for (int s = 1; s < names.size(); ++s) m.addItem (s, names[s]);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&link),
            [this] (int r) { if (r > 0) { proc.armModLink (r); refreshLinkButton(); repaintTop(); }
                             if (restoreFocus) restoreFocus(); });
    }
    void refreshLinkButton()
    {
        const bool armed = proc.modLinkArmedSource() >= 0;
        link.setColour (juce::TextButton::buttonColourId, armed ? kLinkRing : VASynthLookAndFeel::track());
        link.setColour (juce::TextButton::textColourOffId, armed ? VASynthLookAndFeel::panel() : kLinkRing);
        const auto names = ModMatrixPanel::sourceNames();
        const int s = proc.modLinkArmedSource();
        link.setButtonText (armed && s < names.size() ? names[s].toUpperCase() : juce::String ("LINK"));
    }
    void repaintTop() { if (auto* t = getTopLevelComponent()) t->repaint(); }

    void rollRandom()
    {
        juce::Random rng;
        const auto res = proc.randomizeSound (rng);      // NEW: rolls a mode (wild/constrained/archetype)
        currentName = "Random"; refreshTitle(); refreshMacroLabels();
        proc.postToast (res.label);                      // "WILD" / "PAD archetype" / "RANDOM"
        if (auto* t = getTopLevelComponent()) t->repaint();
        if (restoreFocus) restoreFocus();
    }
    void showRandomModeMenu()
    {
        juce::PopupMenu m;
        m.addItem (1, "New: Wild");                       // ASCII-only labels (avoid the String-ctor decode hazard)
        m.addItem (2, "New: Constrained");
        juce::PopupMenu arch;
        for (int i = 0; i < VASynthProcessor::kNumArchetypes; ++i) arch.addItem (100 + i, VASynthProcessor::archetypeName (i));
        m.addSubMenu ("New: Archetype", arch);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&random),
            [this] (int r)
            {
                if (r == 0) { if (restoreFocus) restoreFocus(); return; }
                juce::Random rng;
                using RM = VASynthProcessor::RandomMode;
                const auto res = (r == 1) ? proc.randomizeSound (rng, RM::Wild, -1)
                               : (r == 2) ? proc.randomizeSound (rng, RM::Constrained, -1)
                                          : proc.randomizeSound (rng, RM::Archetype, r - 100);
                currentName = "Random"; refreshTitle(); refreshMacroLabels(); proc.postToast (res.label);
                if (auto* t = getTopLevelComponent()) t->repaint();
                if (restoreFocus) restoreFocus();
            });
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.eventComponent == &random)
        {
            if (e.mods.isPopupMenu()) { suppressRandClick = true; showRandomModeMenu(); return; }
            randDownMs = juce::Time::getMillisecondCounter(); randArmed = true;
        }
    }
    void mouseUp   (const juce::MouseEvent& e) override { if (e.eventComponent == &random) randArmed = false; }
    void mouseDrag (const juce::MouseEvent& e) override { if (e.eventComponent == &random && e.getDistanceFromDragStart() > 8) randArmed = false; }

    void applyMacro (int idx, float value)
    {
        auto id = proc.getMacroTargetId (idx);
        if (id.isEmpty()) return;
        // The "focused part level" macro (#55) resolves live to the edit-focused part's level.
        if (id == VASynthProcessor::kFocusLevelTarget)
            id = "part" + juce::String (juce::jlimit (0, 3, proc.editFocus())) + "_level";
        if (auto* target = proc.apvts.getParameter (id))
            if (! juce::approximatelyEqual (target->getValue(), value))
                target->setValueNotifyingHost (value);
    }
    void refreshMacroLabels()
    {
        for (int m = 0; m < macros.size(); ++m)
        {
            // Label what the macro CONTROLS: single dest -> its short name; multiple -> a user name
            // if set, else "M3 x2"; none -> the plain "M3" fallback. (H3)
            const int n = proc.macroDestinationCount (m);
            const juce::String fallback = "M" + juce::String (m + 1);
            juce::String label;
            if (n == 0)      label = fallback;
            else if (n == 1) label = proc.macroDestinationNames (m).joinIntoString ("");
            else             { const auto custom = proc.macroCustomName (m);
                               label = custom.isNotEmpty() ? custom : (fallback + " x" + juce::String (n)); }
            macros[m]->setDisplayName (label.isNotEmpty() ? label : fallback);
        }
    }

    void renameMacro (int idx)
    {
        auto* aw = new juce::AlertWindow ("Rename Macro " + juce::String (idx + 1),
                                          "A label shown when this macro drives several destinations:",
                                          juce::MessageBoxIconType::NoIcon, this);
        aw->addTextEditor ("name", proc.macroCustomName (idx));
        aw->addButton ("OK",     1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        aw->enterModalState (true, juce::ModalCallbackFunction::create (
            [this, aw, idx] (int r)
            {
                if (r == 1) { proc.setMacroCustomName (idx, aw->getTextEditorContents ("name").trim()); refreshMacroLabels(); }
                if (restoreFocus) restoreFocus();
            }), true);
    }

    void refreshTitle() { presetBtn.setButtonText ("synth  -  " + currentName); }

    void showPresetMenu()
    {
        juce::PopupMenu m;
        int id = 1;
        juce::StringArray order;                       // id -> name
        auto add = [&] (const juce::String& n) { m.addItem (id++, n); order.add (n); };

        add ("Init");
        const auto& lib = proc.factoryPresetLibrary();
        for (auto& cat : lib.categories())
        {
            m.addSectionHeader (cat);
            for (auto& fp : lib.all()) if (fp.category == cat) add (fp.name);
        }
        auto userNames = presets.getPresetNames();
        if (! userNames.isEmpty())
        {
            m.addSectionHeader ("User");
            for (auto& n : userNames) add (n);
        }

        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (presetBtn),
            [this, order] (int r)
            {
                if (r >= 1 && r <= order.size())
                {
                    const auto n = order[r - 1];
                    if (n == "Init")                                           proc.loadInitPreset();
                    else if (proc.factoryPresetLibrary().byName (n) != nullptr) proc.loadFactoryPreset (n);
                    else                                                        proc.loadUserPreset (n);
                    currentName = n; refreshTitle();
                }
                if (restoreFocus) restoreFocus();
            });
    }

    void showSaveDialog()
    {
        auto* aw = new juce::AlertWindow ("Save Preset", "Preset name:",
                                          juce::MessageBoxIconType::NoIcon, this);
        aw->addTextEditor ("name", "");
        aw->addButton ("Save",   1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        aw->enterModalState (true, juce::ModalCallbackFunction::create (
            [this, aw] (int result)
            {
                if (result == 1)
                {
                    const auto n = aw->getTextEditorContents ("name");
                    if (presets.save (n)) { currentName = n; refreshTitle(); }
                }
                if (restoreFocus) restoreFocus();
            }), true);
    }

    VASynthProcessor& proc;
    PresetManager& presets;
    std::function<void()> restoreFocus, toggleHelp, toggleFullscreen;

    juce::TextButton presetBtn, save, random, clear, rec, full, help;
    juce::TextButton vary;                              // H5: perturb the current patch
    juce::TextButton link, mod, inputs, outputs;        // global-action row
    juce::uint32 randDownMs = 0;
    bool randArmed = false, suppressRandClick = false;  // RANDOM long-press / mode-picker state
    bool linkWasArmed = false;
    inline static const juce::Colour kLinkRing { 0xff4bb3c4 };   // LINK cyan (matches the knob armed ring)
    juce::OwnedArray<RotaryKnob> macros;
    juce::OwnedArray<juce::ParameterAttachment> macroAtt;
    std::unique_ptr<RotaryKnob> master, glide, analog;
    std::unique_ptr<HSelector> mode;
    bool voiceCtrlsDisabled = false;   // mode/glide greyed while a kit part is active
    juce::Rectangle<int> statusArea;
    juce::String statusLine { "CPU 0%" }, currentName { "Init" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TopBar)
};
