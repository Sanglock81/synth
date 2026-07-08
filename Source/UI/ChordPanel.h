#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "VASynthLookAndFeel.h"
#include "../PluginProcessor.h"
#include "../DSP/ChordEngine.h"

// ============================================================================
// CHORD section (7B): enable toggle, root + scale selectors, and a grid of the 7
// momentary modifier indicators. Each indicator lights while its modifier is held
// (from any source — QWERTY key / learned CC / learned pad) and is MIDI-learnable:
// long-press / right-click arms it, the next CC (>=64) or note binds, and a badge
// shows the bound source. Refuses keyboard focus (QWERTY note input keeps working).
// ============================================================================

class ChordPanel : public juce::Component,
                   private juce::Timer
{
public:
    ChordPanel (VASynthProcessor& p) : proc (p)
    {
        setWantsKeyboardFocus (false);

        enable.setButtonText ("CHORD");
        enable.setClickingTogglesState (true);
        enable.setWantsKeyboardFocus (false);
        enable.setColour (juce::TextButton::buttonColourId,   VASynthLookAndFeel::track());
        enable.setColour (juce::TextButton::buttonOnColourId, VASynthLookAndFeel::accent());
        enable.setColour (juce::TextButton::textColourOnId,   juce::Colours::black);
        enable.setColour (juce::TextButton::textColourOffId,  VASynthLookAndFeel::dim());
        addAndMakeVisible (enable);
        enableAtt = std::make_unique<juce::ButtonParameterAttachment> (*proc.apvts.getParameter (ParamID::chordEnabled), enable);

        setupCombo (root,  ParamID::chordRoot);
        setupCombo (scale, ParamID::chordScale);
        addAndMakeVisible (root);
        addAndMakeVisible (scale);
        rootAtt  = std::make_unique<juce::ComboBoxParameterAttachment> (*proc.apvts.getParameter (ParamID::chordRoot),  root);
        scaleAtt = std::make_unique<juce::ComboBoxParameterAttachment> (*proc.apvts.getParameter (ParamID::chordScale), scale);

        startTimerHz (20);   // poll held-modifier state for the indicator lights
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6);
        r.removeFromTop (headerH);
        auto top = r.removeFromTop (28);
        enable.setBounds (top.removeFromLeft (top.getWidth() / 3).reduced (2));
        root.setBounds  (top.removeFromLeft (top.getWidth() / 2).reduced (2));
        scale.setBounds (top.reduced (2));
        r.removeFromTop (6);
        const int cellH = 38;
        gridArea = r.removeFromTop (2 * cellH);   // compact 2-row modifier block
        r.removeFromTop (4);
        hintArea = r.removeFromTop (16);          // QWERTY-key hint line
    }

    void paint (juce::Graphics& g) override
    {
        // section frame + header (matches Section)
        auto fr = getLocalBounds().toFloat().reduced (2.0f);
        g.setColour (VASynthLookAndFeel::panelLight().interpolatedWith (tint, 0.10f));
        g.fillRoundedRectangle (fr, 7.0f);
        g.setColour (tint.withAlpha (0.55f));
        g.drawRoundedRectangle (fr, 7.0f, 1.2f);
        g.setColour (VASynthLookAndFeel::ink());
        g.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
        g.drawText ("CHORD", getLocalBounds().removeFromTop (headerH).withTrimmedLeft (10),
                    juce::Justification::centredLeft, false);

        // modifier grid: 7 cells (4 top, 3 bottom)
        const int cols = 4, rows = 2;
        const int cw = gridArea.getWidth() / cols;
        const int ch = gridArea.getHeight() / rows;
        for (int i = 0; i < ChordEngine::kNumModifiers; ++i)
        {
            const int cx = gridArea.getX() + (i % cols) * cw;
            const int cy = gridArea.getY() + (i / cols) * ch;
            juce::Rectangle<float> cell ((float) cx, (float) cy, (float) cw, (float) ch);
            cell.reduce (2.5f, 2.5f);
            cellBounds[(std::size_t) i] = cell.toNearestInt();

            const bool lit      = proc.isModifierActive (i);
            const bool learning = proc.getModifierLearn().isLearningModifier (i);

            g.setColour (lit ? VASynthLookAndFeel::accent()
                             : VASynthLookAndFeel::track());
            g.fillRoundedRectangle (cell, 4.0f);
            if (learning)
            {
                const float a = 0.4f + 0.4f * (float) std::abs (std::sin (juce::Time::getMillisecondCounter() * 0.006));
                g.setColour (VASynthLookAndFeel::accentWarm().withAlpha (a));
                g.drawRoundedRectangle (cell.reduced (1.0f), 4.0f, 2.0f);
            }
            g.setColour (lit ? juce::Colours::black : VASynthLookAndFeel::dim());
            g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
            g.drawText (ChordEngine::modifierName (i), cellBounds[(std::size_t) i], juce::Justification::centred, false);

            // learned-source badge (CC or note)
            const int cc = proc.getModifierLearn().getCCForModifier (i);
            const int nn = proc.getModifierLearn().getNoteForModifier (i);
            if (cc >= 0 || nn >= 0)
            {
                auto badge = cellBounds[(std::size_t) i].removeFromBottom (13).toFloat().reduced (2.0f);
                g.setColour (VASynthLookAndFeel::accentWarm().withAlpha (0.9f));
                g.fillRoundedRectangle (badge, 2.5f);
                g.setColour (juce::Colours::black);
                g.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::bold)));
                g.drawText (cc >= 0 ? ("CC" + juce::String (cc)) : ("N" + juce::String (nn)),
                            badge, juce::Justification::centred, false);
            }
        }

        // QWERTY-key hint (matches the reserved bottom row).
        g.setColour (VASynthLookAndFeel::dim());
        g.setFont (juce::Font (juce::FontOptions (10.5f)));
        g.drawText ("keys  C V B N M , .", hintArea, juce::Justification::centred, false);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        for (int i = 0; i < ChordEngine::kNumModifiers; ++i)
            if (cellBounds[(std::size_t) i].contains (e.getPosition()))
            {
                if (e.mods.isPopupMenu()) { showLearnMenu (i); return; }
                pressCell = i; pressStart = juce::Time::getMillisecondCounter();
                return;
            }
        pressCell = -1;
    }
    void mouseUp (const juce::MouseEvent&) override { pressCell = -1; }

private:
    void setupCombo (juce::ComboBox& c, const juce::String& pid)
    {
        c.setWantsKeyboardFocus (false);
        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter (pid)))
            c.addItemList (choice->choices, 1);
    }

    void showLearnMenu (int modId)
    {
        auto& ml = proc.getModifierLearn();
        juce::PopupMenu m;
        m.addItem (1, "Learn " + juce::String (ChordEngine::modifierName (modId)) + " from a CC/pad");
        if (ml.getCCForModifier (modId) >= 0 || ml.getNoteForModifier (modId) >= 0)
            m.addItem (2, "Clear learned source");
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                         [&ml, modId] (int r)
                         {
                             if (r == 1) ml.armLearn (modId);
                             else if (r == 2) ml.clearModifier (modId);
                         });
    }

    void timerCallback() override
    {
        // Long-press (touch) arms learn, like the fader/knob gesture.
        if (pressCell >= 0 && juce::Time::getMillisecondCounter() - pressStart > 500)
        {
            proc.getModifierLearn().armLearn (pressCell);
            pressCell = -1;
        }
        repaint();
    }

    VASynthProcessor& proc;
    juce::Colour tint { 0xff46c9b0 };   // teal (osc accent)

    juce::TextButton enable;
    juce::ComboBox   root, scale;
    std::unique_ptr<juce::ButtonParameterAttachment>   enableAtt;
    std::unique_ptr<juce::ComboBoxParameterAttachment> rootAtt, scaleAtt;

    juce::Rectangle<int> gridArea, hintArea;
    std::array<juce::Rectangle<int>, ChordEngine::kNumModifiers> cellBounds {};
    int pressCell = -1;
    juce::uint32 pressStart = 0;
    static constexpr int headerH = 24;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChordPanel)
};
