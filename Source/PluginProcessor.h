#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "Parameters.h"
#include "MidiLearnManager.h"
#include "MidiProfile.h"
#include "DSP/SynthEngine.h"
#include "DSP/FXChain.h"
#include "Observability/AudioHealthLogger.h"
#include <atomic>

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

    // -- FX chain order (state-tree property, not an automatable param) --------
    // The editor's drag-reorder calls setFxOrder; it publishes an atomic mirror
    // for the audio thread AND writes the `fx_order` state property so the order
    // saves/loads with presets. `order` must be a permutation of {0,1,2,3}
    // (0=chorus, 1=delay, 2=reverb, 3=width); invalid input is ignored.
    void setFxOrder (const int order[4])
    {
        std::uint32_t packed = 0;
        bool seen[4] { false, false, false, false };
        for (int i = 0; i < 4; ++i)
        {
            const int v = order[i];
            if (v < 0 || v > 3 || seen[v]) return;             // not a permutation -> ignore
            seen[v] = true;
            packed |= (std::uint32_t) v << (i * 8);
        }
        fxOrderPacked.store (packed, std::memory_order_relaxed);
        apvts.state.setProperty (ParamID::fxOrder, orderToString (order), nullptr);
    }

    void getFxOrder (int out[4]) const
    {
        const std::uint32_t packed = fxOrderPacked.load (std::memory_order_relaxed);
        for (int i = 0; i < 4; ++i) out[i] = (int) ((packed >> (i * 8)) & 0xFFu);
    }

    // -- plug-and-play MIDI (the standalone hot-plug watcher drives these) ------
    // Apply the matched device profile's default CC map — factory first, then any
    // user override (user beats factory; a control the user has *learned* is never
    // touched, learned > user > factory). Also adopts the profile's pitch-bend
    // range. No-op if no profile matches the device name.
    void applyDeviceProfile (const juce::String& deviceName);

    // Panic: release all voices on the next block (RT-safe via an atomic flag),
    // so unplugging a controller can't leave a note hanging.
    void requestAllNotesOff() { panicRequested.store (true, std::memory_order_release); }

    // Transient UI notification. The editor polls the sequence number on its timer
    // and shows the latest message as a toast. Message-thread only.
    void postToast (juce::String message) { toastText = std::move (message); toastSeq.fetch_add (1, std::memory_order_release); }
    int  toastSequence() const { return toastSeq.load (std::memory_order_acquire); }
    juce::String toastMessage() const { return toastText; }

    // Directory for user MIDI-profile overrides (*.json). Public for the docs/UI.
    static juce::File userMidiProfileDir();

    // Test seam: build the plugin's binary state format from an XML tree (so the
    // osc_mix->levels migration can be tested with a synthetic pre-level state).
    static void xmlToBinaryForTest (const juce::XmlElement& xml, juce::MemoryBlock& out)
    {
        copyXmlToBinary (xml, out);
    }

    // Computer-keyboard (QWERTY) note input from the standalone editor. Merged
    // into the MIDI stream in processBlock so it flows through the same engine
    // path as hardware MIDI and coexists with it. (In a plugin the host owns the
    // keyboard, so this simply stays silent.)
    juce::MidiKeyboardState qwertyKeyboardState;

    // Audio-health telemetry + RT-safe logging. The editor reads health.snapshot()
    // for the debug overlay.
    AudioHealthLogger health;

private:
    VoiceParams snapshotParams() const;
    FXParams    snapshotFXParams() const;

    // Parse an "a,b,c,d" fx_order property into the atomic mirror (used on load).
    void applyFxOrderProperty();

    static juce::String orderToString (const int order[4])
    {
        return juce::String (order[0]) + "," + juce::String (order[1]) + ","
             + juce::String (order[2]) + "," + juce::String (order[3]);
    }

    // Default order 0,1,2,3 packed one index per byte (byte i = slot i's effect).
    static constexpr std::uint32_t kDefaultOrderPacked = 0x03020100u;

    SynthEngine        engine;
    FXChain            fxChain;
    MidiLearnManager   midiLearn { apvts };
    MidiProfileLibrary profileLib;
    juce::AudioBuffer<float> monoScratch;
    juce::AudioBuffer<float> stereoScratch;
    std::atomic<std::uint32_t> fxOrderPacked { kDefaultOrderPacked };
    std::atomic<bool>  panicRequested { false };
    std::atomic<float> pitchBendRangeSemis { 2.0f };

    // Toast (message-thread only): last message + a monotonically-increasing seq.
    juce::String       toastText;
    std::atomic<int>   toastSeq { 0 };

    // Telemetry bookkeeping (audio thread).
    double        budgetMs   = 2.667;
    std::uint64_t blockIndex = 0;
    std::uint64_t lastSteals = 0;

    // Per-sample master gain ramp to kill zipper on gain steps/automation.
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> masterGain;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VASynthProcessor)
};
