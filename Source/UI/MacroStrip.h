#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "VASynthLookAndFeel.h"
#include "PanelChrome.h"
#include "Widgets.h"
#include "../PluginProcessor.h"

// ============================================================================
// MACRO strip — the 8 performance macros (M1-M8), sitting at the top of the centre
// panel BELOW the top bar (not in it). Relocated here so finger touches never land
// on the native title-bar drag region and get hijacked as window-move/edge gestures
// (defect 1.2). Each macro drives its assigned destination (message-thread routing)
// and shows the destination's name; MIDI-learnable + focus-refusing. The 8 Launchkey
// pots (CC 21-28) map here by default.
// ============================================================================

class MacroStrip : public juce::Component,
                   private juce::Timer
{
public:
    explicit MacroStrip (VASynthProcessor& p) : proc (p)
    {
        setWantsKeyboardFocus (false);
        const char* ids[] { ParamID::macro1, ParamID::macro2, ParamID::macro3, ParamID::macro4,
                            ParamID::macro5, ParamID::macro6, ParamID::macro7, ParamID::macro8 };
        for (int m = 0; m < 8; ++m)
        {
            auto* k = new RotaryKnob (proc.apvts, ids[m], "M" + juce::String (m + 1), proc.getMidiLearn());
            k->setShowValue (false);
            k->setDragPixels ((kDragPixelsForFullRange * 10) / 27);   // ~2.7x responsive
            macros.add (k); addAndMakeVisible (k);
            const int idx = m;
            macroAtt.add (new juce::ParameterAttachment (*proc.apvts.getParameter (ids[m]),
                [this, idx] (float v) { applyMacro (idx, v); }, nullptr));
        }
        refreshLabels();
        startTimerHz (4);   // reflect target-map changes (Random / preset load)
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (VASynthLookAndFeel::panelLight());
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);
        g.setColour (VASynthLookAndFeel::dim());
        g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        g.drawText ("MACROS", labelArea, juce::Justification::centred, false);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8, 6);
        labelArea = r.removeFromLeft (72);
        const int n = macros.size();
        const int kw = juce::jmax (10, r.getWidth() / n);
        for (int m = 0; m < n; ++m)
            macros[m]->setBounds ((m < n - 1 ? r.removeFromLeft (kw) : r).reduced (4, 0));
    }

private:
    void applyMacro (int idx, float value)
    {
        const auto id = proc.getMacroTargetId (idx);
        if (id.isEmpty()) return;
        if (auto* target = proc.apvts.getParameter (id))
            if (! juce::approximatelyEqual (target->getValue(), value))
                target->setValueNotifyingHost (value);
    }
    void refreshLabels()
    {
        for (int m = 0; m < macros.size(); ++m)
        {
            const auto tgt = proc.getMacroTargetName (m);
            macros[m]->setDisplayName (tgt.isNotEmpty() ? tgt : ("M" + juce::String (m + 1)));
        }
    }
    void timerCallback() override { refreshLabels(); }

    VASynthProcessor& proc;
    juce::OwnedArray<RotaryKnob> macros;
    juce::OwnedArray<juce::ParameterAttachment> macroAtt;
    juce::Rectangle<int> labelArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MacroStrip)
};
