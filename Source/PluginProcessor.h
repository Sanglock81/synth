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
#include "DSP/ParametricEQ.h"
#include "DSP/Arpeggiator.h"
#include "DSP/StepSequencer.h"
#include "DSP/Looper.h"
#include "DSP/AudioLoop.h"
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

class VASynthProcessor : public juce::AudioProcessor,
                         private juce::AudioProcessorValueTreeState::Listener
{
public:
    VASynthProcessor();
    ~VASynthProcessor() override;

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
    // Reset all CC bindings to the factory baseline (clears learned/user/profile overrides and
    // restores CC 21-28 -> macro1-8). Recovers the Launchkey macro pots if a stale learn or an
    // old session pointed those CCs elsewhere. Message thread (UI action).
    void resetMidiMappings() { midiLearn.resetToFactoryDefaults(); }
    // Reset the 8 macro -> target assignments to the factory map (M1 cutoff .. M8 focused-part
    // level). Recovers the macros if an old session (or a stray Random) reassigned them. Message
    // thread. Does not move any parameter — only the routing map changes.
    void resetMacroAssignments() { macroTargetId = defaultMacroTargets(); writeMacroMapProperty(); }

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
    // saves/loads with presets. `order` must be a permutation of {0,1,2,3,4}
    // (0=chorus, 1=delay, 2=reverb, 3=width, 4=EQ); invalid input is ignored.
    static constexpr int kFxCount = 5;
    void setFxOrder (const int order[kFxCount])
    {
        std::uint32_t packed = 0;
        bool seen[kFxCount] { };
        for (int i = 0; i < kFxCount; ++i)
        {
            const int v = order[i];
            if (v < 0 || v >= kFxCount || seen[v]) return;     // not a permutation -> ignore
            seen[v] = true;
            packed |= (std::uint32_t) v << (i * 4);            // 4 bits/slot (values 0..4)
        }
        fxOrderPacked.store (packed, std::memory_order_relaxed);
        apvts.state.setProperty (ParamID::fxOrder, orderToString (order), nullptr);
    }

    void getFxOrder (int out[kFxCount]) const
    {
        const std::uint32_t packed = fxOrderPacked.load (std::memory_order_relaxed);
        for (int i = 0; i < kFxCount; ++i) out[i] = (int) ((packed >> (i * 4)) & 0xFu);
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

    // -- kit parts (Sub-phase 1) ----------------------------------------------
    // A KIT part is a per-note pad map. Each pad names a SOURCE preset (baked per pad),
    // a trigger note, its sounding note(s) (2..4 = a chord pad), a level and a choke
    // group. The definition is the editable/serialisable source of truth on the message
    // thread; setPartKit bakes each pad and publishes to the engine.
    struct KitPadDef
    {
        int            triggerNote = -1;                     // -1 = empty pad
        juce::String   source;                               // source preset name (the seed)
        int            soundNote[4] = { 60, 0, 0, 0 };
        int            numSound = 1;
        float          level = 1.0f;
        int            chokeGroup = 0;
        juce::ValueTree voiceState;                          // edited voice (Group 4 kit editing);
                                                             // invalid = bake from `source` (default)
    };
    struct KitDefinition
    {
        juce::String name;
        std::array<KitPadDef, kMaxKitPads> pads {};
    };
    void          setPartKit (int part, const KitDefinition& def);
    bool          isPartKit (int part) const { return part >= 1 && part < SynthEngine::maxParts && engine.partIsKit (part); }
    KitDefinition getPartKit (int part) const { return (part >= 1 && part < SynthEngine::maxParts) ? partKits[(std::size_t) part] : KitDefinition{}; }

    // Kit presets: factory kits (embedded) + user kits (*.kit XML under kitDir). Save
    // serialises the whole definition; load prefers a factory kit, then a user file.
    juce::StringArray getKitNames() const;                                 // factory first, then user
    KitDefinition     loadKit (const juce::String& name) const;            // empty definition if not found
    bool              saveKit (const juce::String& name, const KitDefinition& def);
    static juce::ValueTree   kitToTree   (const KitDefinition& def);        // shared format (kit files + MULTI)
    static KitDefinition     kitFromTree (const juce::ValueTree& t);
    static juce::StringArray factoryKitNames();
    static KitDefinition     factoryKit  (const juce::String& name);

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
    void addSurfaceSplit (const juce::String& surface, int seamNote);      // new boundary at seamNote (new zone = LIVE)
    void removeSurfaceSplit (const juce::String& surface, int zoneIndex);  // merge that zone into a neighbour

    // Route a whole message from a NAMED surface through its zones: notes resolve to a
    // part + transpose (note-off replays the note-on's zone via a ledger); CC/bend/etc
    // pass through globally. Runs off the audio thread (MIDI-callback / message thread).
    void routeSurfaceMessage (const juce::String& surface, const juce::MidiMessage& m);

    // -- MULTI layouts (Part B) -----------------------------------------------
    // A MULTI is a NAMED snapshot of the multitimbral LAYOUT — each locked part's preset
    // plus every surface's zones (ranges/parts/transposes). Since ordinary routing RESETS
    // on relaunch, a MULTI is the only way to recall a layout, and it applies ONLY on an
    // explicit load. Stored as XML under AppInfo::multiDir(). Message thread only.
    juce::ValueTree captureMultiState();                       // shared serialise format (1.3: incl. edits)
    void applyMultiState (const juce::ValueTree& multi);       // a zone on a missing-preset part repoints to LIVE (logged)
    bool saveMulti (const juce::String& name);
    bool loadMulti (const juce::String& name);
    juce::StringArray getMultiNames() const;

    // Per-surface activity counter for the INPUTS dialog's "incoming events" dot.
    // Bumped by the surface's producer (per-input MIDI callback / QWERTY); the dialog
    // polls the count and blinks on a change. Off-audio-thread only.
    void         bumpSurfaceActivity (const juce::String& surface);
    std::uint32_t surfaceActivity (const juce::String& surface) const;
    int          lastNoteForSurface (const juce::String& surface) const;   // -1 if none yet (split-by-play)

    // Last note played on ANY surface + a change sequence — the kit editor's
    // learn-by-play polls these to capture a trigger / sounding note.
    int           lastAnyNote() const { return lastAnyNoteVal.load (std::memory_order_relaxed); }
    std::uint32_t noteSeq()    const { return lastAnyNoteSeq.load (std::memory_order_relaxed); }
    // The trigger note a kit part last fired (pad flicker in the editor).
    int           partLastTrigger (int part) const
    { return (part >= 0 && part < SynthEngine::maxParts) ? partLastTrig[(std::size_t) part].load (std::memory_order_relaxed) : -1; }

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

    // Load a factory preset by name into the FOCUSED part's SOUND only (osc/filter/
    // env/LFO/FX-enable + FX order). All global performance state — sequencer pattern
    // + target, looper, tempo, arp, chord, macros, mixer, EQ, master — and every OTHER
    // part are left untouched, so a patch load never interrupts what's playing. No-op if
    // the name is unknown.
    void loadFactoryPreset (const juce::String& name);

    // Load a USER preset (*.vasynth) into the focused part's SOUND only, same isolation
    // policy as loadFactoryPreset. No-op if the file is missing.
    void loadUserPreset (const juce::String& name);

    // Reset the FOCUSED part's SOUND to Init defaults (FX order -> 0,1,2,3). Globals +
    // other parts untouched (same sound-only isolation as a preset load).
    void loadInitPreset();

    // CLEAR (R3 Group 4): blank the SELECTED part to a clean single sine — resets its
    // sound-design params to default, forces osc1=sine (osc2/3 + noise + FX off). Globals,
    // the mixer and the other parts are untouched (same scope policy as RANDOM).
    void clearFocusedPartToBlank();

    // -- macros (R2) ----------------------------------------------------------
    // Each macro (0..7) can route to one target parameter; the value knob then drives
    // that parameter (applied on the message thread by the editor). The map persists in
    // the apvts state ("macro_map" property), so it saves/loads with presets/sessions.
    // A macro may target the special "focused part level" instead of a fixed param ID: this
    // sentinel resolves at apply-time to the edit-focused part's partN_level (see TopBar).
    static constexpr const char* kFocusLevelTarget = "__focus_level__";
    // Factory macro map (#55): M1 cutoff, M2 reso, M3 filter-env amt, M4 amp release,
    // M5 LFO1 rate, M6 LFO1 depth, M7 reverb mix, M8 focused-part level.
    static std::array<juce::String, 8> defaultMacroTargets();

    juce::String getMacroTargetId (int i) const
    { return (i >= 0 && i < 8) ? macroTargetId[(std::size_t) i] : juce::String(); }
    juce::String getMacroTargetName (int i) const
    {
        const auto id = getMacroTargetId (i);
        if (id == kFocusLevelTarget) return "Focus Level";
        if (auto* p = id.isNotEmpty() ? apvts.getParameter (id) : nullptr) return p->getName (16);
        return {};
    }
    void setMacroTarget (int i, const juce::String& paramId)
    {
        if (i < 0 || i >= 8) return;
        macroTargetId[(std::size_t) i] = paramId;
        writeMacroMapProperty();
    }
    // Randomize: assign 1..4 macros (100/50/25/10% for 1/2/3/4) each to a DISTINCT
    // routable parameter and randomize their values. Called by the Random button.
    void randomizeMacros (juce::Random& rng);
    // Curated musical parameters a macro may target.
    static juce::StringArray macroRoutableIDs();
    // The per-part SOUND-design parameter IDs (osc/filter/env/LFO/FX). RANDOM shuffles
    // ONLY these — of the selected part — leaving the mixer, EQ, macros and every other
    // global/performance control alone.
    static const juce::StringArray& soundDesignParamIDs();

    // -- arpeggiator 16-step pattern (R3; per-step velocity #54) ----------------
    // Each step has an on/off ("arp_steps") AND a velocity percent ("arp_vel", 10..200,
    // 0/100 = default). Identical model + interaction to the step sequencer's cells/vel:
    // tap toggles the step, hold-drag sets its velocity. Both live in the state tree so
    // they save/load with presets/MULTIs; the RHYTHM panel edits them. Message + UI thread.
    static constexpr int kArpSteps = Arpeggiator::kNumSteps;
    float getArpStep (int i) const { return (i >= 0 && i < kArpSteps) ? arpSteps[(std::size_t) i] : 0.0f; }
    void  setArpStep (int i, float v)
    {
        if (i < 0 || i >= kArpSteps) return;
        arpSteps[(std::size_t) i] = (v > 0.5f) ? 1.0f : 0.0f;
        writeArpStepsProperty();
    }
    // Per-step velocity PERCENT (10..200; 0 or 100 = default). Emitted vel = min(1, played*%/100).
    int  getArpStepVel (int i) const
    { const int v = (i >= 0 && i < kArpSteps) ? arpStepVel[(std::size_t) i] : 0; return v == 0 ? 100 : v; }
    void setArpStepVel (int i, int velPercent)
    { if (i >= 0 && i < kArpSteps) { arpStepVel[(std::size_t) i] = (unsigned char) juce::jlimit (0, 200, velPercent); writeArpStepsProperty(); } }

    // Live LFO modulation on the focused part for a destination (1 pitch semis, 2 cutoff
    // octaves, 3 pw units) — the UI animates the CUTOFF / PW knobs from this.
    float lfoModForDest (int dest) const { return engine.focusModForDest (dest); }
    int   activeVoicesForPart (int part) const { return engine.activeVoiceCountForPart (part); }
    // The arp's currently-playing step (-1 = idle) for the sequencer playhead. UI polls it.
    int arpDisplayStep() const { return arpStepDisp.load (std::memory_order_relaxed); }

    // -- step sequencer (R3 Group 2): 8-row x 16-step drum grid ---------------
    // Pattern (cells / per-row trigger note / mute) lives in the state tree so it saves
    // with presets + MULTIs. The RHYTHM/SEQ panel edits it. Message thread + UI.
    static constexpr int kSeqRows = StepSequencer::kRows, kSeqSteps = StepSequencer::kSteps;
    // Cell on/off (0 = off, 1 = on). Legacy value 2 (accent) is accepted -> on + velocity 127
    // (the accent is now a per-step velocity, task #54). getSeqCell still returns 2 for an
    // accented (vel > 100) step so older callers/UX read the same.
    unsigned char getSeqCell (int row, int step) const
    {
        if (row < 0 || row >= kSeqRows || step < 0 || step >= kSeqSteps || seqCells[(std::size_t) row][(std::size_t) step] == 0) return 0;
        return seqVel[(std::size_t) row][(std::size_t) step] > 100 ? 2 : 1;
    }
    void setSeqCell (int row, int step, unsigned char v)
    {
        if (row < 0 || row >= kSeqRows || step < 0 || step >= kSeqSteps) return;
        seqCells[(std::size_t) row][(std::size_t) step] = (v == 0) ? 0 : 1;
        if (v == 2) seqVel[(std::size_t) row][(std::size_t) step] = 127;   // legacy accent -> high velocity
        writeSeqProperty();
    }
    // Per-step velocity PERCENT (10..200; 0 or 100 = default). Emitted vel = min(1, %/100).
    int  getSeqStepVel (int row, int step) const
    { const int v = (row >= 0 && row < kSeqRows && step >= 0 && step < kSeqSteps) ? seqVel[(std::size_t) row][(std::size_t) step] : 0; return v == 0 ? 100 : v; }
    void setSeqStepVel (int row, int step, int velPercent)
    { if (row >= 0 && row < kSeqRows && step >= 0 && step < kSeqSteps) { seqVel[(std::size_t) row][(std::size_t) step] = (unsigned char) juce::jlimit (0, 200, velPercent); writeSeqProperty(); } }
    int  getSeqNote (int row) const { return (row >= 0 && row < kSeqRows) ? seqNotes[(std::size_t) row] : 0; }
    void setSeqNote (int row, int note)
    { if (row >= 0 && row < kSeqRows) { seqNotes[(std::size_t) row] = juce::jlimit (0, 127, note); writeSeqProperty(); } }
    bool getSeqMute (int row) const { return row >= 0 && row < kSeqRows && seqMutes[(std::size_t) row]; }
    void setSeqMute (int row, bool m) { if (row >= 0 && row < kSeqRows) { seqMutes[(std::size_t) row] = m; writeSeqProperty(); } }
    int  seqDisplayStep() const { return seqStepDisp.load (std::memory_order_relaxed); }

    // -- edit focus (1.3): which part the panel edits / the LIVE surface plays --------
    // The focused part is the APVTS-driven (live, smoothed) part; the other three play
    // from their baked states. Tapping a part swaps the whole panel to its state (the UI
    // calls setEditFocus). Focus 0 is the default and is bit-identical to the old model.
    int  editFocus() const { return editFocusPart.load (std::memory_order_relaxed); }
    int  playFocus() const { return playFocusPart.load (std::memory_order_relaxed); }   // which part the LIVE keyboard plays
    // Focus a part (0..3): the LIVE keyboard PLAYS it (so a kit part triggers its pads).
    // A SYNTH part also becomes the panel's EDIT focus — save the current part's sound, load
    // the tapped part's sound into the APVTS (panel refreshes). A KIT part has no single
    // panel sound, so the panel keeps editing the current synth part (edit the kit in the
    // Kit Editor); only play-focus moves. Message thread (UI).
    void setEditFocus (int part);
    // Has this locked part diverged from its assigned preset (shows the "(edited)" tag)?
    bool partIsEdited (int part) const
    { return part >= 0 && part < SynthEngine::maxParts && partEdited[(std::size_t) part]; }
    // Revert a locked part to its assigned preset (undo panel edits), symmetric with how
    // the edit began. Message thread.
    void revertPartToPreset (int part);

    // Kit per-pad voice editing (Group 4 increment B). beginKitPadEdit swaps the main panel
    // to the pad's voice (the kit part becomes a live synth so you play + edit + hear it);
    // endKitPadEdit(true) bakes the panel state back into the pad and restores the kit +
    // previous focus. Message thread. Returns false if the pad is empty / not a kit part.
    bool beginKitPadEdit (int part, int pad);
    void endKitPadEdit   (bool commit);
    bool isEditingKitPad () const { return kitPadEditActive.load (std::memory_order_acquire); }
    int  editingKitPart  () const { return kitPadEditPart; }
    int  editingKitPad   () const { return kitPadEditPad; }

    // -- looper (R3) ----------------------------------------------------------
    // Runtime loop content (recorded notes) is NOT preset material; it's exported to
    // MIDI. The UI reads the playhead + per-lane content for drawing. Message thread.
    void  clearLoops() { loopClearMask.store ((1 << SynthEngine::maxParts) - 1, std::memory_order_release); }        // all lanes
    void  clearLoopLane (int part) { if (part >= 0 && part < SynthEngine::maxParts) loopClearMask.fetch_or (1 << part, std::memory_order_release); }
    float loopPlayhead() const                                                 // 0..1 through the loop
    {
        const int len = loopLenDisp.load (std::memory_order_relaxed);
        return len > 0 ? (float) loopPosDisp.load (std::memory_order_relaxed) / (float) len : 0.0f;
    }
    bool  loopLaneHasContent (int part) const { return looper.hasContent (part); }
    int   loopLaneEventCount (int part) const { return looper.eventCount (part); }
    // Honest audio cap: how many bars (1/2/4) the audio ring can hold at the current tempo.
    // Below ~40 BPM a 4-bar loop exceeds the ring, so AUDIO mode is limited (shown in the UI).
    int   maxAudioLoopBars() const
    {
        const double bpm = juce::jmax (20.0f, apvts.getRawParameterValue (ParamID::tempo)->load());
        const double sr  = getSampleRate() > 0.0 ? getSampleRate() : 48000.0;
        const double barSamples = sr * 60.0 / bpm * 4.0;                 // 4 beats/bar
        const double ring = sr * AudioLoop::kMaxLoopSeconds;
        const int fit = (int) (ring / juce::jmax (1.0, barSamples));     // whole bars that fit
        return fit >= 4 ? 4 : fit >= 2 ? 2 : 1;
    }
    bool  loopAudioHasContent (int part) const { return part >= 0 && part < SynthEngine::maxParts && audioLoops[(std::size_t) part].hasContent(); }
    int   loopRecDisplayState (int lane) const { return (lane >= 0 && lane < SynthEngine::maxParts) ? loopRecStateDisp[(std::size_t) lane].load (std::memory_order_relaxed) : 0; }
    // Write the recorded loops to a Standard MIDI File (one track per part). Returns false
    // if there's nothing recorded or the write fails.
    bool  exportLoopsToMidiFile (const juce::File& file) const;
    // Write the recorded AUDIO lane to a stereo WAV. Returns false if the audio lane is
    // empty or the write fails.
    bool  exportLoopToWavFile (const juce::File& file) const;

    // Test seam: build the plugin's binary state format from an XML tree (so the
    // osc_mix->levels migration can be tested with a synthetic pre-level state).
    static void xmlToBinaryForTest (const juce::XmlElement& xml, juce::MemoryBlock& out)
    {
        copyXmlToBinary (xml, out);
    }

    // Audio-health telemetry + RT-safe logging. The editor reads health.snapshot()
    // for the debug overlay.
    AudioHealthLogger health;

    // -- master scope tap (RT-safe SPSC ring) ---------------------------------
    // The audio thread appends the post-master mono mix each block (relaxed atomic
    // stores — no lock/alloc); the editor's scope + FFT read the latest samples. Not
    // sample-exact under contention (it's a scope, not a meter), which is fine.
    static constexpr int kScopeSize = 2048;                 // power of two
    void pushScope (const float* l, const float* r, int n) noexcept
    {
        int w = scopeWrite.load (std::memory_order_relaxed);
        for (int i = 0; i < n; ++i)
            scopeRing[(std::size_t) (w++ & (kScopeSize - 1))].store (0.5f * (l[i] + r[i]), std::memory_order_relaxed);
        scopeWrite.store (w, std::memory_order_release);
    }
    // Copy the latest n samples (oldest..newest) into dst. Message thread.
    void readScope (float* dst, int n) const noexcept
    {
        n = juce::jmin (n, (int) kScopeSize);
        const int w = scopeWrite.load (std::memory_order_acquire);
        for (int i = 0; i < n; ++i)
            dst[i] = scopeRing[(std::size_t) ((w - n + i) & (kScopeSize - 1))].load (std::memory_order_relaxed);
    }

    // The LIVE (focused-part) baked voice params — read-only, for tests/diagnostics that
    // need the resolved DSP parameters (e.g. verifying a preset routes velocity to cutoff).
    VoiceParams currentVoiceParams() const { return snapshotParams(); }

private:
    VoiceParams snapshotParams() const;
    FXParams    snapshotFXParams() const;
    // Bake a source preset -> VoiceParams (+ optionally its FX + 3 LFOs). Shared by
    // locked parts (fxOut/lfoOut set) and kit pads (null). ok=false if preset missing.
    VoiceParams bakePresetParams (const juce::String& name, bool& ok,
                                  FXParams* fxOut = nullptr, PartLfos* lfoOut = nullptr,
                                  juce::ValueTree* stateOut = nullptr);
    // Bake VoiceParams from an edited voice state tree (a kit pad's own sound). Same
    // scratch-APVTS kill-fold as bakePresetParams, so an edited pad is bit-identical to
    // loading that sound live.
    VoiceParams bakeVoiceStateParams (const juce::ValueTree& state);

    // Feed the combined (QWERTY | MIDI) held-modifier mask into the chord engine's
    // latest-wins forcer stack as edges (audio thread).
    void applyChordModifiers (std::uint32_t combinedMask);

    // Note dispatch shared by the host `midi` buffer (part 0) and the routed FIFO:
    // part 0 goes through the chord engine; locked parts play the note directly.
    void dispatchNoteOn  (int note, float velocity, int part, bool chordOn, bool generator = false);
    void dispatchNoteOff (int note, int part, bool chordOn);
    void flushLoopNotes  (bool chordOn);              // release notes the MIDI loop left on (all lanes)
    void flushLoopNotesForPart (int part, bool chordOn);   // ...one lane (#47)

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
    void drainRoutedMidi (bool chordOn, int focus);   // audio thread (focus = LIVE part remap, 1.3)
    void handleControlMessage (const juce::MidiMessage& m); // CC/pitch-bend/all-off, shared

    // Parse an "a,b,c,d" fx_order property into the atomic mirror (used on load).
    void applyFxOrderProperty();

    // Macro routing map <-> the "macro_map" state property (persists with presets).
    std::array<juce::String, 8> macroTargetId {};
    void writeMacroMapProperty();
    void applyMacroMapProperty();

    // Arpeggiator step pattern <-> "arp_steps" (on/off) + "arp_vel" (per-step %) state props.
    std::array<float, kArpSteps> arpSteps { };
    std::array<unsigned char, kArpSteps> arpStepVel { };   // per-step velocity % (0 = default 100)
    void writeArpStepsProperty();
    void applyArpStepsProperty();

    // Step sequencer grid <-> "seq_cells" / "seq_notes" / "seq_mutes" state properties.
    std::array<std::array<unsigned char, kSeqSteps>, kSeqRows> seqCells { };
    std::array<std::array<unsigned char, kSeqSteps>, kSeqRows> seqVel { };   // per-step velocity % (0 = default 100)
    std::array<int, kSeqRows>  seqNotes { { 36, 37, 38, 39, 40, 41, 42, 43 } };
    std::array<bool, kSeqRows> seqMutes { };
    void writeSeqProperty();
    void applySeqProperty();

    static juce::String orderToString (const int order[kFxCount])
    {
        juce::String s;
        for (int i = 0; i < kFxCount; ++i) s += (i ? "," : "") + juce::String (order[i]);
        return s;
    }

    // Default order 0,1,2,3,4 packed 4 bits per slot (slot i in nibble i).
    static constexpr std::uint32_t kDefaultOrderPacked = 0x43210u;

    SynthEngine        engine;          // owns the per-part voices + per-part FX (Sub-phase 2)
    ChordEngine        chordEngine;
    MidiLearnManager    midiLearn { apvts };
    ModifierLearnManager modifierLearn;
    MidiProfileLibrary  profileLib;
    FactoryPresetLibrary factoryPresets;
    juce::AudioBuffer<float> stereoScratch;   // master L/R sum target
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
    std::array<KitDefinition, SynthEngine::maxParts> partKits {};   // per-part kit definition (message thread)

    // Edit focus (1.3). editFocusPart = the part the APVTS currently represents (panel +
    // engine live slot). partEditState holds the OTHER parts' full panel states; on a
    // focus swap they exchange with the APVTS. partEdited marks divergence from a preset.
    std::atomic<int> editFocusPart { 0 };   // panel + engine live-param slot (synth parts)
    std::atomic<int> playFocusPart { 0 };   // which part the LIVE keyboard routes to (any part)
    std::array<juce::ValueTree, SynthEngine::maxParts> partEditState {};

    // Kit per-pad edit mode (Group 4 B): while active, kitPadEditPart is a live synth showing
    // the pad's voice; on end it's re-baked into the kit and the saved focus restored.
    std::atomic<bool> kitPadEditActive { false };
    int kitPadEditPart = 1, kitPadEditPad = 0, kitPadEditSavedFocus = 0;
    std::array<bool, SynthEngine::maxParts> partEdited {};
    bool loadingPartState = false;   // guards the edited-flag listener during programmatic loads
    void parameterChanged (const juce::String& id, float newValue) override;   // marks a focused locked part edited
    void bakeStateToSlot (int part, const juce::ValueTree& state);   // state tree -> VoiceParams/FX/LFO -> engine slot
    void applyPartSoundFromTree (const juce::ValueTree& tree);       // copy sound params (+fx order) -> live APVTS
    void applyFocusedPartSound (const juce::ValueTree& soundTree);   // load a SOUND into the focused part; globals + other parts untouched
    void syncFocusedPartState();                                     // capture the focused part's live sound into its store
    void applyDefaultScene();                                        // startup layout: P2=808 kit (seq target), P3=bass, P4=spare
    bool defaultSceneApplied = false;                                // applyDefaultScene runs once, on the first prepareToPlay

    juce::CriticalSection routingLock;
    std::vector<std::pair<juce::String, std::uint32_t>> surfaceHits; // surface -> activity count
    std::vector<std::pair<juce::String, int>> surfaceNotes;         // surface -> last note (split-by-play)

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
    std::array<std::atomic<int>, SynthEngine::maxParts> partLastTrig {};         // kit editor pad flicker
    std::atomic<int>           lastAnyNoteVal { -1 };                            // learn-by-play (any surface)
    std::atomic<std::uint32_t> lastAnyNoteSeq { 0 };

    // Toast (message-thread only): last message + a monotonically-increasing seq.
    juce::String       toastText;
    std::atomic<int>   toastSeq { 0 };

    // Telemetry bookkeeping (audio thread).
    double        budgetMs   = 2.667;
    std::uint64_t blockIndex = 0;
    std::uint64_t lastSteals = 0;

    // Per-sample master gain ramp to kill zipper on gain steps/automation.
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> masterGain;

    // Master parametric EQ (end of chain, post-FX sum / pre master gain). Audio thread only.
    ParametricEQ masterEQ;
    bool eqWasOn = false;

    // Arpeggiator (audio thread). When enabled it captures the LIVE part's played notes
    // and re-emits them on its internal clock. A fixed per-block event buffer keeps the
    // emit path alloc-free.
    Arpeggiator arp;
    StepSequencer seq;
    // Internal note events for a block from the arp + step sequencer. `viaDispatch` picks
    // the sink: true -> dispatchNoteOn/Off (kit-aware, for the sequencer's target part);
    // false -> engine.noteOn/off directly (the arp on the live part).
    struct GenEvent { int offset; int note; float vel; bool on; int part; bool viaDispatch; bool generator; };
    std::array<GenEvent, 1024> genEv { };
    int genEvCount = 0;
    bool arpWasOn = false;
    bool seqWasOn = false;                  // seq enable edge -> flush held notes on disable
    int  prevSeqTarget = 3;                 // seq target-change edge -> release the old part's note (default P4 = kit)
    int  prevBarIdx = -1;                   // shared-transport bar index (#53): re-anchor arp/seq at each bar
    bool chordWasOn = false;               // chord enable edge -> release held tones on disable
    std::atomic<int> seqStepDisp { -1 };   // audio -> UI: sequencer playhead
    int  prevPlayFocus = 0;                // audio-thread mirror of play-focus, for hand-off
    std::atomic<int> arpStepDisp { -1 };   // audio -> UI: current arp step (playhead)

    // Looper (audio thread). Playback dispatches at block start; recording captures the
    // performance input in drain/host. Display mirrors + a CLEAR request are atomics.
    // Group 3: a parallel AUDIO lane records the play-focused part's post-FX (captured by
    // the engine); loop_mode picks which lane you HEAR. Recording is armed + quantized to
    // the loop boundary (a measure): REC arms, capture engages at the next wrap.
    Looper looper;
    std::array<AudioLoop, SynthEngine::maxParts> audioLoops;   // one AUDIO lane per part (#47)
    std::atomic<int>  loopClearMask { 0 };      // bit per lane: RT-safe per-lane CLEAR request
    std::atomic<int>  loopPosDisp { 0 }, loopLenDisp { 48000 };
    std::array<std::atomic<int>, SynthEngine::maxParts> loopRecStateDisp {};   // per lane: 0 idle,1 armed,2 rec
    // Per-lane transport edge/state (audio-thread only).
    std::array<bool, SynthEngine::maxParts> loopRecPrev {};      // REC param edge detect
    std::array<bool, SynthEngine::maxParts> loopArmPending {};   // armed, waiting for the loop boundary
    std::array<bool, SynthEngine::maxParts> loopRecording {};    // capture engaged (both lanes of this part)
    std::array<bool, SynthEngine::maxParts> loopRecJustEngaged {};   // engage block (don't count its wrap)
    std::array<bool, SynthEngine::maxParts> loopPlayWasOn {};    // MIDI-lane playback edge, to flush on stop
    bool  loopWrappedLastBlock = false;         // the shared loop clock wrapped on the previous block
    std::array<std::array<bool, 128>, SynthEngine::maxParts> loopNoteHeld {};   // notes the loop turned on (release on stop/clear)

    // Master scope tap ring (see pushScope/readScope).
    std::array<std::atomic<float>, kScopeSize> scopeRing {};
    std::atomic<int> scopeWrite { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VASynthProcessor)
};
