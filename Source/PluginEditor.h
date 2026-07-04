#pragma once
#include "PluginProcessor.h"

// ============================================================================
// v1 GUI strategy: wrap JUCE's GenericAudioProcessorEditor, which builds a
// slider/combo for every APVTS parameter automatically. Zero UI code to
// maintain while the engine stabilizes.
//
// v2: replace with a custom Component layout (oscillator / filter / envelope
// panels, MIDI-learn right-click on any control). Nothing in the engine or
// processor changes when we do — that's the point of keeping this thin.
// ============================================================================

class VASynthEditor : public juce::AudioProcessorEditor
{
public:
    explicit VASynthEditor (VASynthProcessor& p)
        : AudioProcessorEditor (p), genericEditor (p)
    {
        addAndMakeVisible (genericEditor);
        setSize (520, 700);
        setResizable (true, true);
    }

    void resized() override
    {
        genericEditor.setBounds (getLocalBounds());
    }

private:
    juce::GenericAudioProcessorEditor genericEditor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VASynthEditor)
};
