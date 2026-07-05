#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "Parameters.h"
#include "MidiLearnManager.h"
#include "DSP/SynthEngine.h"

// ============================================================================
// The AudioProcessor is the seam between JUCE-land and our engine:
//
//   MIDI in ──> processBlock ──> event dispatch (sample-accurate)
//                                  │
//   APVTS ────> param snapshot ────┼──> SynthEngine::render (mono)
//                                  │         │
//                                  └──> MidiLearnManager (CCs)
//                                            │
//                              copy mono -> L/R out
//
// JUCE builds this same class into both the VST3 and the standalone app.
// In standalone, JUCE's wrapper provides the audio-device / MIDI-input
// settings dialog (that's where you pick the Scarlett 2i2 and enable both
// the Korg B2 and Launchkey Mini).
// ============================================================================

class VASynthProcessor : public juce::AudioProcessor
{
public:
    VASynthProcessor();

    // -- lifecycle -----------------------------------------------------------
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    // Float-only synth; keep the base double-precision overload in scope so it
    // isn't hidden (-Woverloaded-virtual). supportsDoublePrecisionProcessing()
    // is false by default, so the double version is never actually called.
    using juce::AudioProcessor::processBlock;

    // -- editor ----------------------------------------------------------------
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    // -- boilerplate -------------------------------------------------------------
    const juce::String getName() const override { return "VA Synth"; }
    bool acceptsMidi() const override  { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    // -- state (presets / session recall) ------------------------------------
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    // Exposed for the (v2) right-click MIDI-learn GUI and for tests.
    MidiLearnManager& getMidiLearn() { return midiLearn; }

    // Computer-keyboard (QWERTY) note input from the standalone editor. Merged
    // into the MIDI stream in processBlock so it flows through the same engine
    // path as hardware MIDI and coexists with it. (In a plugin the host owns the
    // keyboard, so this simply stays silent.)
    juce::MidiKeyboardState qwertyKeyboardState;

private:
    VoiceParams snapshotParams() const;

    SynthEngine      engine;
    MidiLearnManager midiLearn { apvts };
    juce::AudioBuffer<float> monoScratch;

    // Per-sample master gain ramp to kill zipper on gain steps/automation.
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> masterGain;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VASynthProcessor)
};
