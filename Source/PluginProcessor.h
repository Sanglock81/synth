#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "Parameters.h"
#include "MidiLearnManager.h"
#include "ModifierLearnManager.h"
#include "MidiProfile.h"
#include "FactoryPresets.h"
#include "DSP/SynthEngine.h"
#include "DSP/ChordEngine.h"
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
    const juce::String getName() const override { return "synth"; }
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

    // -- chord engine (7B) -----------------------------------------------------
    // The editor publishes which QWERTY chord-modifier keys are down as a bitmask
    // (bit i = ChordEngine::ModifierId i). RT-safe: the audio thread diffs it each
    // block and feeds the edges into the chord engine's latest-wins forcer stack.
    void setQwertyChordModifiers (std::uint32_t mask) { qwertyModMask.store (mask, std::memory_order_release); }
    ModifierLearnManager& getModifierLearn() { return modifierLearn; }
    // For the CHORD UI indicators: is this modifier currently active (any source)?
    bool isModifierActive (int modId) const { return (activeModMask.load (std::memory_order_acquire) >> modId) & 1u; }

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

    // -- parts / multitimbral (7C) --------------------------------------------
    // Bake a preset into a LOCKED part (1..3) on the message thread and publish it
    // to the audio thread. name "" or "Init" -> Init baseline; a missing user preset
    // -> Init with a logged warning (never a crash). Part 0 is always LIVE (edited
    // by the panel) and cannot be assigned.
    bool setPartPreset (int part, const juce::String& presetName);   // false if the named preset was missing (baked Init)
    juce::String getPartPreset (int part) const
    {
        return (part >= 1 && part < SynthEngine::maxParts) ? partPresetName[(std::size_t) part] : juce::String();
    }

    // -- key-range zones (Part B) ---------------------------------------------
    // A surface's keyboard is split into an ordered, contiguous, non-overlapping list
    // of zones tiling [0,127]. Each zone routes its note range to a part and transposes
    // the sounding note by `transpose` semitones (the trigger note is unchanged; the
    // result is clamped to MIDI). Default (no config) = one full-range LIVE zone. POD.
    struct Zone { int loNote = 0, hiNote = 127, part = 0, transpose = 0; };

    // A playing surface (QWERTY or a MIDI input by name) -> a part index (0 = LIVE).
    // Convenience over zones: assigns the WHOLE surface to one part (collapses any
    // split). Zones/routing RESET to default on relaunch (only MULTI load recalls them).
    void setSurfaceRouting (const juce::String& surface, int part);
    int  getSurfaceRouting (const juce::String& surface) const;   // single-zone part; covering-60 part if split

    // Full zone list for a surface (empty vector => the implicit default LIVE zone).
    std::vector<Zone> getSurfaceZones (const juce::String& surface) const;
    void setSurfaceZones (const juce::String& surface, std::vector<Zone> zones);   // validated/normalised
    bool surfaceHasSplit (const juce::String& surface) const;      // more than one zone
    void resetSurfaceZones (const juce::String& surface);          // back to a single full-range LIVE zone
    void resetAllRouting();                                        // every surface -> default

    // Route a whole message from a NAMED surface through its zones: notes resolve to a
    // part + transpose (note-off replays the note-on's zone via a ledger); CC/bend/etc
    // pass through globally. Runs off the audio thread (MIDI-callback / message thread).
    void routeSurfaceMessage (const juce::String& surface, const juce::MidiMessage& m);

    // -- MULTI layouts (Part B) -----------------------------------------------
    // A MULTI is a NAMED snapshot of the multitimbral LAYOUT — each locked part's preset
    // plus every surface's zones (ranges/parts/transposes). Since ordinary routing RESETS
    // on relaunch, a MULTI is the only way to recall a layout, and it applies ONLY on an
    // explicit load. Stored as XML under AppInfo::multiDir(). Message thread only.
    juce::ValueTree captureMultiState() const;                 // shared serialise format
    void applyMultiState (const juce::ValueTree& multi);       // a zone on a missing-preset part repoints to LIVE (logged)
    bool saveMulti (const juce::String& name);
    bool loadMulti (const juce::String& name);
    juce::StringArray getMultiNames() const;

    // Per-surface activity counter for the INPUTS dialog's "incoming events" dot.
    // Bumped by the surface's producer (per-input MIDI callback / QWERTY); the dialog
    // polls the count and blinks on a change. Off-audio-thread only.
    void         bumpSurfaceActivity (const juce::String& surface);
    std::uint32_t surfaceActivity (const juce::String& surface) const;

    // Per-part note-activity counter (audio thread bumps; UI polls) — drives the
    // PARTS strip's per-part flicker so multitimbrality is visible at a glance.
    std::uint32_t partActivity (int part) const
    {
        return (part >= 0 && part < SynthEngine::maxParts) ? partHits[(std::size_t) part].load (std::memory_order_relaxed) : 0;
    }

    // Route a note from a surface to its part. RT-safe multi-producer push (message
    // thread QWERTY + MIDI-thread per-device callbacks); drained in processBlock.
    void routeNoteOn  (int note, float velocity, int part) { pushRouted (0x90, note, (int) (velocity * 127.0f + 0.5f), part); }
    void routeNoteOff (int note, int part)                 { pushRouted (0x80, note, 0, part); }

    // Route a whole MIDI message (<=3 bytes: note/CC/pitch-bend) from a surface. The
    // standalone's per-input callbacks use this so a routed device's CCs, pitch bend
    // and sustain reach the synth (control messages are handled globally / on the
    // live part; only notes carry the part). Bumps the surface's activity counter.
    void routeMidi (const juce::MidiMessage& m, int part)
    {
        const auto* d = m.getRawData();
        const int n = m.getRawDataSize();
        if (n >= 1) pushRouted (d[0], n >= 2 ? d[1] : 0, n >= 3 ? d[2] : 0, part);
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

    // -- factory presets (read-only, embedded) --------------------------------
    const FactoryPresetLibrary& factoryPresetLibrary() const { return factoryPresets; }

    // Load a factory preset by name: reset to Init, apply its overrides, and set
    // its FX order (default if unspecified). No-op if the name is unknown.
    void loadFactoryPreset (const juce::String& name);

    // Reset every parameter to its default and the FX order to 0,1,2,3 ("Init").
    void loadInitPreset();

    // Test seam: build the plugin's binary state format from an XML tree (so the
    // osc_mix->levels migration can be tested with a synthetic pre-level state).
    static void xmlToBinaryForTest (const juce::XmlElement& xml, juce::MemoryBlock& out)
    {
        copyXmlToBinary (xml, out);
    }

    // Audio-health telemetry + RT-safe logging. The editor reads health.snapshot()
    // for the debug overlay.
    AudioHealthLogger health;

private:
    VoiceParams snapshotParams() const;
    FXParams    snapshotFXParams() const;

    // Feed the combined (QWERTY | MIDI) held-modifier mask into the chord engine's
    // latest-wins forcer stack as edges (audio thread).
    void applyChordModifiers (std::uint32_t combinedMask);

    // Note dispatch shared by the host `midi` buffer (part 0) and the routed FIFO:
    // part 0 goes through the chord engine; locked parts play the note directly.
    void dispatchNoteOn  (int note, float velocity, int part, bool chordOn);
    void dispatchNoteOff (int note, int part, bool chordOn);

    // -- routed-MIDI FIFO (surfaces -> parts) ---------------------------------
    // Each event is a raw <=3-byte MIDI message + the routed part. Notes carry the
    // part; control messages are handled globally on drain.
    struct RoutedEvent { std::uint8_t status, d1, d2, part; };
    void pushRouted (int status, int d1, int d2, int part)   // producers (non-audio)
    {
        if (part < 0 || part >= SynthEngine::maxParts) return;
        const juce::SpinLock::ScopedLockType sl (routedPushLock);
        int start1, size1, start2, size2;
        routedFifo.prepareToWrite (1, start1, size1, start2, size2);
        if (size1 > 0)
        {
            routedBuf[(std::size_t) start1] = { (std::uint8_t) status, (std::uint8_t) d1, (std::uint8_t) d2, (std::uint8_t) part };
            routedFifo.finishedWrite (1);
        }   // full -> drop (never blocks a producer)
    }
    void drainRoutedMidi (bool chordOn);              // audio thread
    void handleControlMessage (const juce::MidiMessage& m); // CC/pitch-bend/all-off, shared

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
    ChordEngine        chordEngine;
    FXChain            fxChain;
    MidiLearnManager    midiLearn { apvts };
    ModifierLearnManager modifierLearn;
    MidiProfileLibrary  profileLib;
    FactoryPresetLibrary factoryPresets;
    juce::AudioBuffer<float> monoScratch;
    juce::AudioBuffer<float> stereoScratch;
    std::atomic<std::uint32_t> fxOrderPacked { kDefaultOrderPacked };
    std::atomic<bool>  panicRequested { false };
    std::atomic<float> pitchBendRangeSemis { 2.0f };

    // Chord-modifier state: QWERTY mask published by the editor (message thread),
    // diffed on the audio thread; activeModMask mirrors the resolved held modifiers
    // back for the UI indicators.
    std::atomic<std::uint32_t> qwertyModMask { 0 };   // message thread -> audio
    std::atomic<std::uint32_t> activeModMask { 0 };   // audio -> UI (held modifiers)
    std::uint32_t midiModMask   = 0;   // audio-thread: modifiers held via CC/note
    std::uint32_t lastFedModMask = 0;  // audio-thread: mask currently in the chord engine

    // Parts (7C): locked-part preset names (message thread; persisted) + a routing
    // table (surface name -> part). Routing/preset changes happen on the message
    // thread; the baked params reach the audio thread via the engine's double buffer.
    std::array<juce::String, SynthEngine::maxParts> partPresetName {};
    juce::CriticalSection routingLock;
    std::vector<std::pair<juce::String, std::uint32_t>> surfaceHits; // surface -> activity count

    // Key-range zones (Part B): surface name -> ordered zone list tiling [0,127]. Plus a
    // note-off ledger (surface,note) -> the part+sounding-note its note-on resolved to, so
    // a note-off releases exactly what its note-on triggered even if the zones changed
    // mid-hold (ledger philosophy). Both are touched only off the audio thread (MIDI
    // callback / message thread) under zoneLock — the audio thread sees only resolved
    // events via the FIFO, so it never takes this lock.
    struct LedgerEntry { juce::String surface; int note, part, sounding; };
    mutable juce::CriticalSection zoneLock;
    std::vector<std::pair<juce::String, std::vector<Zone>>> surfaceZones;
    std::vector<LedgerEntry> noteLedger;

    Zone resolveZone (const juce::String& surface, int note) const; // zoneLock held by caller-safe wrapper

    // Routed-MIDI FIFO: surfaces (QWERTY / per-device MIDI) push part-tagged note
    // events; processBlock drains them. Multi-producer push serialised by a SpinLock
    // (producers are non-audio threads); the audio thread reads lock-free.
    static constexpr int kRoutedCapacity = 2048;
    juce::AbstractFifo routedFifo { kRoutedCapacity };
    std::array<RoutedEvent, kRoutedCapacity> routedBuf {};
    juce::SpinLock routedPushLock;
    std::array<std::atomic<std::uint32_t>, SynthEngine::maxParts> partHits {};   // per-part note flicker

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
