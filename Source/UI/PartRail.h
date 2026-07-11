#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "VASynthLookAndFeel.h"
#include "PanelChrome.h"
#include "Widgets.h"
#include "KitEditor.h"
#include "InputsDialog.h"
#include "../PluginProcessor.h"
#include "../DSP/SynthEngine.h"

// ============================================================================
// Left PART RAIL: one cell per part (P1 = LIVE, P2-P4 = locked/kit), each with an
// activity dot, its assignment (live / preset name / kit pad grid / "tap to add")
// and a per-part LEVEL fader on the right. Tap a locked/empty cell to open its Kit
// editor. Refuses keyboard focus; the level faders are grab-mode + focus-refusing.
//
// R2 note: per-part PAN and MIDI-learnable level (the old MIX section) still live in
// the state/MULTI and automation — a dedicated on-panel mixer detail is a follow-up.
// ============================================================================

class PartRail : public juce::Component,
                 private juce::Timer
{
public:
    PartRail (VASynthProcessor& p, std::function<void()> restoreFocusFn)
        : proc (p), restoreFocus (std::move (restoreFocusFn))
    {
        setWantsKeyboardFocus (false);
        const char* lvlIds[] { ParamID::part0Level, ParamID::part1Level, ParamID::part2Level, ParamID::part3Level };
        const char* panIds[] { ParamID::part0Pan,   ParamID::part1Pan,   ParamID::part2Pan,   ParamID::part3Pan   };
        for (int i = 0; i < SynthEngine::maxParts; ++i)
        {
            lvl[(std::size_t) i] = std::make_unique<RotaryKnob> (proc.apvts, lvlIds[i], "LVL", proc.getMidiLearn());
            pan[(std::size_t) i] = std::make_unique<RotaryKnob> (proc.apvts, panIds[i], "PAN", proc.getMidiLearn());
            addAndMakeVisible (*lvl[(std::size_t) i]);
            addAndMakeVisible (*pan[(std::size_t) i]);
        }

        inputs.setButtonText ("INPUTS");
        inputs.setWantsKeyboardFocus (false);
        inputs.setColour (juce::TextButton::buttonColourId, VASynthLookAndFeel::track());
        inputs.setColour (juce::TextButton::textColourOffId, VASynthLookAndFeel::ink());
        inputs.onClick = [this] { InputsDialog::show (proc, getTopLevelComponent(), [this] { if (restoreFocus) restoreFocus(); }); };
        addAndMakeVisible (inputs);

        startTimerHz (15);
    }

    void paint (juce::Graphics& g) override
    {
        auto rl = chrome::section (g, getLocalBounds(), "Parts", juce::Colour (0xff9aa3ad));
        auto cells = cellRects (rl);
        const auto tChord = juce::Colour (0xffe0733a);

        for (int i = 0; i < SynthEngine::maxParts; ++i)
        {
            auto cell = cells[(std::size_t) i];
            const bool live = (i == 0);
            const bool lit  = blink[(std::size_t) i] > 0;
            const bool locked = i > 0 && proc.getPartPreset (i).isNotEmpty();
            const bool kit    = proc.isPartKit (i);

            g.setColour (lit ? VASynthLookAndFeel::track().brighter (0.28f)
                             : (live ? VASynthLookAndFeel::track().brighter (0.10f) : VASynthLookAndFeel::track()));
            g.fillRoundedRectangle (cell.toFloat(), 6.0f);
            if (live) { g.setColour (VASynthLookAndFeel::accent().withAlpha (0.9f)); g.drawRoundedRectangle (cell.toFloat().reduced (1), 6.0f, 2.0f); }

            cell.removeFromRight (kKnobCol);           // level + pan knob column (laid out in resized)
            auto body = cell.reduced (7, 5);

            g.setColour ((live || lit) ? VASynthLookAndFeel::accent() : VASynthLookAndFeel::dim());
            g.fillEllipse (body.removeFromLeft (11).removeFromTop (11).toFloat());
            body.removeFromLeft (2);

            juce::String name = "P" + juce::String (i + 1) + (live ? "  LIVE" : "");
            juce::String sub;
            if (live)        sub = "live patch";
            else if (kit)    sub = "kit  -  " + juce::String (kitPadCount (i)) + " pads";
            else if (locked) sub = proc.getPartPreset (i);
            else           { name = "P" + juce::String (i + 1); sub = "tap to add"; }

            g.setColour (VASynthLookAndFeel::ink());
            g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
            g.drawText (name, body.removeFromTop (16).withTrimmedLeft (4), juce::Justification::centredLeft, false);
            g.setColour (VASynthLookAndFeel::dim());
            g.setFont (juce::Font (juce::FontOptions (10.0f)));
            g.drawText (sub, body.removeFromTop (13).withTrimmedLeft (4), juce::Justification::centredLeft, false);

            if (kit)                                    // kit-pad sub-selector seam
            {
                auto grid = body.withTrimmedTop (2);
                const int n = kitPadCount (i);
                for (int pd = 0; pd < 8; ++pd)
                {
                    auto pad = juce::Rectangle<int> (grid.getX() + 3 + (pd % 4) * (grid.getWidth() / 4),
                                                     grid.getY() + (pd / 4) * (grid.getHeight() / 2),
                                                     grid.getWidth() / 4, grid.getHeight() / 2).reduced (2);
                    g.setColour (pd < n ? tChord.withAlpha (0.75f) : VASynthLookAndFeel::track().darker (0.2f));
                    g.fillRoundedRectangle (pad.toFloat(), 3.0f);
                }
            }
        }
    }

    void resized() override
    {
        inputs.setBounds (getLocalBounds().removeFromTop (chrome::kHeaderH).removeFromRight (92).reduced (3, 3));
        auto rl = chrome::sectionContent (getLocalBounds());
        auto cells = cellRects (rl);
        for (int i = 0; i < SynthEngine::maxParts; ++i)
        {
            auto col = cells[(std::size_t) i].removeFromRight (kKnobCol).reduced (2, 6);
            lvl[(std::size_t) i]->setBounds (col.removeFromLeft (col.getWidth() / 2).reduced (1, 0));
            pan[(std::size_t) i]->setBounds (col.reduced (1, 0));
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        auto cells = cellRects (chrome::sectionContent (getLocalBounds()));
        for (int i = 1; i < SynthEngine::maxParts; ++i)          // P1 (LIVE) edited on the panel
            if (cells[(std::size_t) i].withTrimmedRight (kKnobCol).contains (e.getPosition()))
            { KitEditor::show (proc, getTopLevelComponent(), i, [this] { if (restoreFocus) restoreFocus(); }); return; }
    }

private:
    std::array<juce::Rectangle<int>, SynthEngine::maxParts> cellRects (juce::Rectangle<int> rl) const
    {
        const int n = SynthEngine::maxParts, gap = 5;
        const int cellH = juce::jmax (20, (rl.getHeight() - (n - 1) * gap) / n);
        std::array<juce::Rectangle<int>, SynthEngine::maxParts> out;
        for (int i = 0; i < n; ++i) { out[(std::size_t) i] = rl.removeFromTop (cellH); rl.removeFromTop (gap); }
        return out;
    }

    int kitPadCount (int part) const
    {
        int n = 0;
        for (auto& pad : proc.getPartKit (part).pads) if (pad.triggerNote >= 0) ++n;
        return n;
    }

    void timerCallback() override
    {
        for (int i = 0; i < SynthEngine::maxParts; ++i)
        {
            const auto now = proc.partActivity (i);
            if (now != lastHits[(std::size_t) i]) { lastHits[(std::size_t) i] = now; blink[(std::size_t) i] = 3; }
            else if (blink[(std::size_t) i] > 0)  --blink[(std::size_t) i];
        }
        repaint();
    }

    static constexpr int kKnobCol = 100;   // per-cell level+pan knob column width

    VASynthProcessor& proc;
    std::function<void()> restoreFocus;
    juce::TextButton inputs;
    std::array<std::unique_ptr<RotaryKnob>, SynthEngine::maxParts> lvl, pan;
    std::array<std::uint32_t, SynthEngine::maxParts> lastHits {};
    std::array<int, SynthEngine::maxParts> blink {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PartRail)
};
