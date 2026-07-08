#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "VASynthLookAndFeel.h"
#include "InputsDialog.h"
#include "../PluginProcessor.h"
#include "../DSP/SynthEngine.h"

// ============================================================================
// Slim, always-visible PARTS strip (proof-of-life for multitimbrality; the full
// part rail is a future GUI item). Four cells — P0 LIVE and P1-P3 — each showing
// its assignment (a dash when unused, the locked preset name otherwise) and
// flickering on per-part note activity. An INPUTS button at the right opens the
// routing dialog. Refuses keyboard focus (QWERTY keeps working).
// ============================================================================

class PartsStrip : public juce::Component,
                   private juce::Timer
{
public:
    PartsStrip (VASynthProcessor& p, std::function<void()> restoreFocus)
        : proc (p), restore (std::move (restoreFocus))
    {
        setWantsKeyboardFocus (false);
        inputs.setButtonText ("INPUTS");
        inputs.setWantsKeyboardFocus (false);
        inputs.setColour (juce::TextButton::buttonColourId, VASynthLookAndFeel::accent().darker (0.2f));
        inputs.setColour (juce::TextButton::textColourOffId, juce::Colours::black);
        inputs.onClick = [this] { InputsDialog::show (proc, getTopLevelComponent(), [this] { if (restore) restore(); }); };
        addAndMakeVisible (inputs);
        startTimerHz (15);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (3, 3);
        inputs.setBounds (r.removeFromRight (120).reduced (2, 1));
        labelArea = r.removeFromLeft (52);
        cellArea  = r;
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (VASynthLookAndFeel::panelLight());
        g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 6.0f);
        g.setColour (VASynthLookAndFeel::ink());
        g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
        g.drawText ("PARTS", labelArea, juce::Justification::centred, false);

        const int n = SynthEngine::maxParts;
        const int cw = cellArea.getWidth() / n;
        for (int i = 0; i < n; ++i)
        {
            auto cell = juce::Rectangle<int> (cellArea.getX() + i * cw, cellArea.getY(), cw, cellArea.getHeight()).reduced (3, 3);
            const bool lit = blink[(std::size_t) i] > 0;
            const bool locked = i > 0 && proc.getPartPreset (i).isNotEmpty();

            g.setColour (lit ? VASynthLookAndFeel::accent()
                             : (locked ? VASynthLookAndFeel::track().brighter (0.10f) : VASynthLookAndFeel::track()));
            g.fillRoundedRectangle (cell.toFloat(), 4.0f);

            juce::String label = "P" + juce::String (i) + "  ";
            label += (i == 0) ? juce::String ("LIVE") : (locked ? proc.getPartPreset (i) : juce::String ("--"));
            g.setColour (lit ? juce::Colours::black : (locked ? VASynthLookAndFeel::ink() : VASynthLookAndFeel::dim()));
            g.setFont (juce::Font (juce::FontOptions (12.0f, i == 0 ? juce::Font::bold : juce::Font::plain)));
            g.drawText (label, cell.reduced (7, 0), juce::Justification::centredLeft, true);
        }
    }

private:
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

    VASynthProcessor& proc;
    std::function<void()> restore;
    juce::TextButton inputs;
    juce::Rectangle<int> labelArea, cellArea;
    std::array<std::uint32_t, SynthEngine::maxParts> lastHits {};
    std::array<int, SynthEngine::maxParts> blink {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PartsStrip)
};
