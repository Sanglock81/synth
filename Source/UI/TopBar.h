#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "VASynthLookAndFeel.h"
#include "Widgets.h"
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
        random.onClick = [this]
        {
            juce::Random rng;
            presets.randomize (rng);
            proc.randomizeMacros (rng);        // assign 1-4 macros to random params + values
            currentName = "Random"; refreshTitle(); refreshMacroLabels();
            if (restoreFocus) restoreFocus();
        };
        addAndMakeVisible (random);

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

        const char* ids[] { ParamID::macro1, ParamID::macro2, ParamID::macro3, ParamID::macro4,
                            ParamID::macro5, ParamID::macro6, ParamID::macro7, ParamID::macro8 };
        for (int m = 0; m < 8; ++m)
        {
            auto* k = new RotaryKnob (proc.apvts, ids[m], "M" + juce::String (m + 1), proc.getMidiLearn());
            macros.add (k); addAndMakeVisible (k);
            // When a macro moves, drive its assigned target (message-thread routing).
            const int idx = m;
            macroAtt.add (new juce::ParameterAttachment (*proc.apvts.getParameter (ids[m]),
                [this, idx] (float v) { applyMacro (idx, v); }, nullptr));
        }

        master = std::make_unique<RotaryKnob> (proc.apvts, ParamID::masterGain, "MASTER", proc.getMidiLearn());
        addAndMakeVisible (*master);

        refreshMacroLabels();
        startTimerHz (4);   // CPU readout + macro-label resync (map may change on preset load)
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

        auto preset = tb.removeFromLeft (250); tb.removeFromLeft (8);
        auto pTop = preset.removeFromTop (34);
        presetBtn.setBounds (pTop.removeFromLeft (150).reduced (0, 1));
        save.setBounds   (pTop.removeFromLeft (48).reduced (2, 1));
        random.setBounds (pTop.reduced (2, 1));
        statusArea = preset;

        auto help_ = tb.removeFromRight (34); tb.removeFromRight (4);
        help.setBounds (help_.reduced (0, 16));
        full.setBounds (tb.removeFromRight (36).reduced (0, 16)); tb.removeFromRight (6);
        master->setBounds (tb.removeFromRight (92)); tb.removeFromRight (4);
        rec.setBounds (tb.removeFromRight (54).reduced (0, 16)); tb.removeFromRight (10);

        // 8 macro knobs, packed together (fixed width) and centred in the free span
        // between the preset area and the right cluster.
        const int mw = juce::jmin (86, tb.getWidth() / 8);
        const int pack = mw * 8;
        tb.removeFromLeft (juce::jmax (0, (tb.getWidth() - pack) / 2));   // centre the group
        for (int m = 0; m < macros.size(); ++m)
            macros[m]->setBounds (tb.removeFromLeft (mw));
    }

    // Update the shown patch name from outside (e.g. a factory load elsewhere).
    void setCurrentName (juce::String n) { currentName = std::move (n); refreshTitle(); }

private:
    void timerCallback() override
    {
        const int cpu = (int) juce::roundToInt (proc.health.snapshot().cpuPercent);
        auto line = "CPU " + juce::String (juce::jlimit (0, 999, cpu)) + "%";
        if (line != statusLine) { statusLine = line; repaint (statusArea); }
        refreshMacroLabels();     // map can change on preset/session load
    }

    // Drive a macro's assigned target parameter (message thread).
    void applyMacro (int idx, float value)
    {
        const auto id = proc.getMacroTargetId (idx);
        if (id.isEmpty()) return;
        if (auto* target = proc.apvts.getParameter (id))
            if (! juce::approximatelyEqual (target->getValue(), value))
                target->setValueNotifyingHost (value);
    }

    // Macro knob shows its assigned target's short name (or M1..M8 when unassigned).
    void refreshMacroLabels()
    {
        for (int m = 0; m < macros.size(); ++m)
        {
            const auto tgt = proc.getMacroTargetName (m);
            macros[m]->setDisplayName (tgt.isNotEmpty() ? tgt : ("M" + juce::String (m + 1)));
        }
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
                    else                                                        presets.load (n);
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

    juce::TextButton presetBtn, save, random, rec, full, help;
    juce::OwnedArray<RotaryKnob> macros;
    juce::OwnedArray<juce::ParameterAttachment> macroAtt;
    std::unique_ptr<RotaryKnob> master;
    juce::Rectangle<int> statusArea;
    juce::String statusLine { "CPU 0%" }, currentName { "Init" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TopBar)
};
