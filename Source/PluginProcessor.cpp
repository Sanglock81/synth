#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "AppInfo.h"
#include "DSP/SoftClip.h"
#include <BinaryData.h>
#include <chrono>
#include <algorithm>
#include <vector>
#include <utility>

#ifndef VASYNTH_VERSION
 #define VASYNTH_VERSION "?"
#endif
#ifndef VASYNTH_GIT_HASH
 #define VASYNTH_GIT_HASH "?"
#endif
#ifndef VASYNTH_BUILD_TYPE
 #define VASYNTH_BUILD_TYPE "?"
#endif

// Flush the log with a final marker on a crash so post-mortem logs show where
// things stopped. The AudioHealthLogger installs its FileLogger as the current
// JUCE logger, so writeToLog reaches the same file (and flushes).
static void vaSynthCrashHandler (void*)
{
    juce::Logger::writeToLog ("*** VA SYNTH CRASH - application crash handler ***");
    juce::Logger::writeToLog (juce::SystemStats::getStackBacktrace());
}

VASynthProcessor::VASynthProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createParameterLayout())
{
    juce::SystemStats::setApplicationCrashHandler (vaSynthCrashHandler);

    // One-time migration of an existing rig's config (presets + MIDI profiles)
    // from the pre-rename "VASynth" location to "synth". No-op once migrated or on
    // a fresh machine. Runs before we read the profile/preset dirs below.
    AppInfo::migrateLegacyConfig();

    // Factory MIDI device profiles (embedded), plus any user overrides on disk.
    profileLib.addFactory (juce::String::fromUTF8 (BinaryData::launchkey_mini_json, BinaryData::launchkey_mini_jsonSize));
    profileLib.addFactory (juce::String::fromUTF8 (BinaryData::korg_b2_json,        BinaryData::korg_b2_jsonSize));
    profileLib.loadUserDir (userMidiProfileDir());

    // Factory presets: every embedded JSON that parses as a preset (a device
    // profile lacks a "params" object, so it's skipped). Sort by original filename
    // so the numeric "01_.." prefixes give a stable menu order.
    {
        std::vector<std::pair<juce::String, juce::String>> byFile;   // (filename, json)
        for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
        {
            int size = 0;
            if (const char* data = BinaryData::getNamedResource (BinaryData::namedResourceList[i], size))
            {
                bool ok = false;
                FactoryPreset::fromJson (juce::String::fromUTF8 (data, size), ok);
                if (ok) byFile.emplace_back (BinaryData::originalFilenames[i], juce::String::fromUTF8 (data, size));
            }
        }
        std::sort (byFile.begin(), byFile.end(),
                   [] (const auto& a, const auto& b) { return a.first < b.first; });
        for (auto& kv : byFile) factoryPresets.add (kv.second);
    }
}

void VASynthProcessor::loadFactoryPreset (const juce::String& name)
{
    const auto* p = factoryPresets.byName (name);
    if (p == nullptr) return;
    const auto keep = PresetPolicy::capture (apvts);   // preset is a sound, not a level
    p->applyParams (apvts);
    PresetPolicy::restore (apvts, keep);
    if (p->fxOrder.size() == 4) { int o[4]; for (int i = 0; i < 4; ++i) o[i] = p->fxOrder[i]; setFxOrder (o); }
    else { const int def[4] { 0, 1, 2, 3 }; setFxOrder (def); }
}

void VASynthProcessor::loadInitPreset()
{
    // Init resets the SOUND to defaults but keeps the player's global performance
    // controls (master level) put — same policy as factory/user load.
    const auto keep = PresetPolicy::capture (apvts);
    for (auto* rp : getParameters()) rp->setValueNotifyingHost (rp->getDefaultValue());
    PresetPolicy::restore (apvts, keep);
    const int def[4] { 0, 1, 2, 3 };
    setFxOrder (def);
}

juce::File VASynthProcessor::userMidiProfileDir()
{
    return AppInfo::midiProfileDir();
}

// Apply the matched profile's default CC map. Factory first, then user, so user
// overrides factory per-CC; MidiLearnManager's precedence keeps learned mappings
// untouched (learned > user > factory).
void VASynthProcessor::applyDeviceProfile (const juce::String& deviceName)
{
    using Src = MidiLearnManager::Source;
    if (auto* fac = profileLib.factoryFor (deviceName))
    {
        for (auto& m : fac->mappings) midiLearn.applyProfileMapping (m.first, m.second, Src::Factory);
        pitchBendRangeSemis.store ((float) fac->pitchBendRange, std::memory_order_release);
    }
    if (auto* usr = profileLib.userFor (deviceName))
    {
        for (auto& m : usr->mappings) midiLearn.applyProfileMapping (m.first, m.second, Src::User);
        pitchBendRangeSemis.store ((float) usr->pitchBendRange, std::memory_order_release);
    }
}

// Oscillator anti-aliasing quality. Compile-time default is Efficient (glitch-
// free with headroom on the 2-core live ThinkPad); build with
// -DVASYNTH_OSC_QUALITY_HQ for the studio/Windows HQ default. A runtime GUI
// selector (re-preparing voices off the audio thread) is a v2 item.
#ifdef VASYNTH_OSC_QUALITY_HQ
 #define VASYNTH_OSC_QUALITY PolyBlepOscillator::Quality::HQ
#else
 #define VASYNTH_OSC_QUALITY PolyBlepOscillator::Quality::Efficient
#endif

// Max simultaneous voices. 16 (the full pool) — chosen so a future keyboard
// SPLIT can layer two independently-voiced 8-voice sounds. This exceeds the ~30%
// ThinkPad comfort target in the pathological worst case (16 held x 3 saws + all
// FX ~= 41% derated median, p99 ~65%), but stays well under the 100% real-time
// limit (no dropouts); realistic split playing sits far lower. Reviewed and
// accepted for the split-voicing roadmap. Drop back to 12 to reclaim headroom.
#ifndef VASYNTH_MAX_VOICES
 #define VASYNTH_MAX_VOICES 16
#endif

void VASynthProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.setOscQuality (VASYNTH_OSC_QUALITY);
    engine.setMaxVoices (VASYNTH_MAX_VOICES);
    engine.prepare (sampleRate);
    // Allocate the mono mixdown + stereo FX buffers ONCE, at the host's max block
    // size. processBlock never resizes them (JUCE guarantees numSamples <= this).
    monoScratch.setSize (1, juce::jmax (1, samplesPerBlock), false, false, true);
    stereoScratch.setSize (2, juce::jmax (1, samplesPerBlock), false, false, true);
    fxChain.prepare (sampleRate, juce::jmax (1, samplesPerBlock));

    masterGain.reset (sampleRate, 0.02);                       // ~20 ms ramp
    masterGain.setCurrentAndTargetValue (
        apvts.getRawParameterValue (ParamID::masterGain)->load());

    budgetMs = (sampleRate > 0.0) ? (double (samplesPerBlock) / sampleRate * 1000.0) : 2.667;

    // Startup banner (processor-accessible fields; device/type/MIDI-inputs are
    // logged by the standalone app which owns the device manager).
    const char* quality =
        (VASYNTH_OSC_QUALITY == PolyBlepOscillator::Quality::HQ)   ? "HQ"
      : (VASYNTH_OSC_QUALITY == PolyBlepOscillator::Quality::None) ? "None" : "Efficient";
    // Build provenance so a stale binary is obvious: git hash (configure time) +
    // this TU's compile timestamp (updates on every rebuild) + build type.
    health.logMessage (juce::String ("synth ") + VASYNTH_VERSION + " (git " + VASYNTH_GIT_HASH
                       + ", built " __DATE__ " " __TIME__ ", " + VASYNTH_BUILD_TYPE + ")"
                       + "  wrapper=" + juce::String ((int) wrapperType)
                       + "  osc-quality=" + quality
                       + "  maxVoices=" + juce::String (VASYNTH_MAX_VOICES)
                       + "  parts=" + juce::String (SynthEngine::maxParts));
    health.prepare (sampleRate, samplesPerBlock);
}

// Helper: read a float parameter's current value from the APVTS.
// getRawParameterValue returns an atomic<float>* — lock-free, audio-safe.
static float rp (const juce::AudioProcessorValueTreeState& s, const char* id)
{
    return s.getRawParameterValue (id)->load();
}

// Build a VoiceParams from ANY APVTS carrying our layout. Used both for the LIVE
// snapshot (this->apvts, audio thread) and to BAKE a preset into a locked part (a
// scratch APVTS, message thread) — reusing the exact kill-switch fold so a locked
// part sounds bit-identical to loading that preset live (7C).
static VoiceParams buildVoiceParams (const juce::AudioProcessorValueTreeState& apvts)
{
    namespace ID = ParamID;
    VoiceParams p;

    p.osc1Wave   = (int) rp (apvts, ID::osc1Wave);
    p.osc2Wave   = (int) rp (apvts, ID::osc2Wave);
    p.osc3Wave   = (int) rp (apvts, ID::osc3Wave);
    p.osc1Octave = rp (apvts, ID::osc1Octave);
    p.osc2Octave = rp (apvts, ID::osc2Octave);
    p.osc3Octave = rp (apvts, ID::osc3Octave);
    p.osc1Detune = rp (apvts, ID::osc1Detune);
    p.osc2Detune = rp (apvts, ID::osc2Detune);
    p.osc3Detune = rp (apvts, ID::osc3Detune);
    p.osc1PW     = rp (apvts, ID::osc1PW);
    p.osc2PW     = rp (apvts, ID::osc2PW);
    p.osc3PW     = rp (apvts, ID::osc3PW);
    p.oscMix     = rp (apvts, ID::oscMix);        // legacy; unused by the engine
    p.noiseLevel = rp (apvts, ID::noiseLevel);

    // Fold each kill switch into its level (off -> 0); the engine smooths these
    // effective levels, so toggling is click-free and off oscillators are skipped.
    p.osc1Level  = rp (apvts, ID::osc1On) > 0.5f ? rp (apvts, ID::osc1Level) : 0.0f;
    p.osc2Level  = rp (apvts, ID::osc2On) > 0.5f ? rp (apvts, ID::osc2Level) : 0.0f;
    p.osc3Level  = rp (apvts, ID::osc3On) > 0.5f ? rp (apvts, ID::osc3Level) : 0.0f;

    p.velToAmp    = rp (apvts, ID::velToAmp);
    p.velToCutoff = rp (apvts, ID::velToCutoff);

    p.filterType   = (int) rp (apvts, ID::filterType);
    p.cutoffHz     = rp (apvts, ID::filterCutoff);
    p.resonance    = rp (apvts, ID::filterReso);
    p.filterEnvAmt = rp (apvts, ID::filterEnvAmt);
    p.keytrack     = rp (apvts, ID::filterKeytrack);

    p.ampA = rp (apvts, ID::ampAttack);
    p.ampD = rp (apvts, ID::ampDecay);
    p.ampS = rp (apvts, ID::ampSustain);
    p.ampR = rp (apvts, ID::ampRelease);

    p.fltA = rp (apvts, ID::fltAttack);
    p.fltD = rp (apvts, ID::fltDecay);
    p.fltS = rp (apvts, ID::fltSustain);
    p.fltR = rp (apvts, ID::fltRelease);
    p.fltEnvToPitch = rp (apvts, ID::fltEnvToPitch);

    p.glideTime = rp (apvts, ID::glideTime);

    return p;
}

VoiceParams VASynthProcessor::snapshotParams() const { return buildVoiceParams (apvts); }

void VASynthProcessor::applyChordModifiers (std::uint32_t combined)
{
    const std::uint32_t changed = combined ^ lastFedModMask;
    for (int i = 0; i < ChordEngine::kNumModifiers; ++i)
        if ((changed >> i) & 1u)
            chordEngine.setModifierHeld (i, (combined >> i) & 1u);
    lastFedModMask = combined;
}

// ---- parts (7C) -----------------------------------------------------------
// A throwaway AudioProcessor that exists ONLY to host an APVTS with our layout, so
// a preset can be applied on the message thread and baked into a VoiceParams for a
// locked part — without disturbing the live processor's state or engine.
namespace
{
    struct BakeProcessor : juce::AudioProcessor
    {
        juce::AudioProcessorValueTreeState apvts { *this, nullptr, "PARAMS", createParameterLayout() };
        const juce::String getName() const override { return "bake"; }
        void prepareToPlay (double, int) override {}
        void releaseResources() override {}
        void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
        double getTailLengthSeconds() const override { return 0.0; }
        bool acceptsMidi() const override { return false; }
        bool producesMidi() const override { return false; }
        juce::AudioProcessorEditor* createEditor() override { return nullptr; }
        bool hasEditor() const override { return false; }
        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram (int) override {}
        const juce::String getProgramName (int) override { return {}; }
        void changeProgramName (int, const juce::String&) override {}
        void getStateInformation (juce::MemoryBlock&) override {}
        void setStateInformation (const void*, int) override {}
    };

    void bakeInitBaseline (BakeProcessor& b)
    {
        for (auto* rp : b.getParameters()) rp->setValueNotifyingHost (rp->getDefaultValue());
    }
}

void VASynthProcessor::setPartPreset (int part, const juce::String& name)
{
    if (part < 1 || part >= SynthEngine::maxParts) return;

    BakeProcessor scratch;
    bool ok = true;
    if (name.isEmpty() || name == "Init")
        bakeInitBaseline (scratch);
    else if (const auto* fp = factoryPresets.byName (name))
        fp->applyParams (scratch.apvts);
    else if (auto xml = juce::XmlDocument::parse (AppInfo::presetDir().getChildFile (name + ".vasynth")))
    {
        auto tree = juce::ValueTree::fromXml (*xml);
        const bool needsMigration = stateNeedsLevelMigration (tree);
        scratch.apvts.replaceState (tree);
        if (needsMigration) applyLegacyOscLevelMigration (scratch.apvts);
    }
    else
    {
        ok = false;                                 // missing user preset -> Init fallback
        bakeInitBaseline (scratch);
        juce::Logger::writeToLog ("part " + juce::String (part) + " preset '" + name + "' missing -> Init");
    }

    engine.setLockedPartParams (part, buildVoiceParams (scratch.apvts));
    partPresetName[(std::size_t) part] = ok ? name : juce::String ("Init");
}

void VASynthProcessor::setSurfaceRouting (const juce::String& surface, int part)
{
    if (part < 0 || part >= SynthEngine::maxParts) return;
    const juce::ScopedLock sl (routingLock);
    for (auto& e : routingTable) if (e.first == surface) { e.second = part; return; }
    routingTable.emplace_back (surface, part);
}

int VASynthProcessor::getSurfaceRouting (const juce::String& surface) const
{
    const juce::ScopedLock sl (routingLock);
    for (auto& e : routingTable) if (e.first == surface) return e.second;
    return 0;                                        // default: LIVE
}

void VASynthProcessor::bumpSurfaceActivity (const juce::String& surface)
{
    const juce::ScopedLock sl (routingLock);
    for (auto& e : surfaceHits) if (e.first == surface) { ++e.second; return; }
    surfaceHits.emplace_back (surface, 1u);
}

std::uint32_t VASynthProcessor::surfaceActivity (const juce::String& surface) const
{
    const juce::ScopedLock sl (routingLock);
    for (auto& e : surfaceHits) if (e.first == surface) return e.second;
    return 0;
}

void VASynthProcessor::dispatchNoteOn (int note, float vel, int part, bool chordOn)
{
    if (part >= 0 && part < SynthEngine::maxParts)
        partHits[(std::size_t) part].fetch_add (1, std::memory_order_relaxed);   // PARTS strip flicker

    if (part == 0 && chordOn)
    {
        const int mod = modifierLearn.handleNoteOn (note);   // learned pad consumed
        if (mod >= 0)
        {
            midiModMask |= (1u << mod);
            applyChordModifiers (qwertyModMask.load (std::memory_order_acquire) | midiModMask);
            return;
        }
        int trig[ChordEngine::kMaxTones], rel[ChordEngine::kMaxTones]; int nt = 0, nr = 0;
        chordEngine.noteOn (note, trig, nt, rel, nr);
        for (int i = 0; i < nr; ++i) engine.noteOff (rel[i], 0);
        for (int i = 0; i < nt; ++i) engine.noteOn (trig[i], vel, 0);
    }
    else
        engine.noteOn (note, vel, part);              // locked parts: chord is live-only
}

void VASynthProcessor::dispatchNoteOff (int note, int part, bool chordOn)
{
    if (part == 0 && chordOn)
    {
        const int mod = modifierLearn.handleNoteOff (note);
        if (mod >= 0)
        {
            midiModMask &= ~(1u << mod);
            applyChordModifiers (qwertyModMask.load (std::memory_order_acquire) | midiModMask);
            return;
        }
        int rel[ChordEngine::kMaxTones]; int nr = 0;
        chordEngine.noteOff (note, rel, nr);
        for (int i = 0; i < nr; ++i) engine.noteOff (rel[i], 0);
    }
    else
        engine.noteOff (note, part);
}

// CC / pitch-bend / all-notes-off — shared by the host `midi` buffer and the routed
// FIFO. These act globally / on the live part (v1: control is not per-part).
void VASynthProcessor::handleControlMessage (const juce::MidiMessage& msg)
{
    if (msg.isAllNotesOff() || msg.isAllSoundOff())
    {
        engine.allNotesOff(); chordEngine.reset(); midiModMask = 0; lastFedModMask = 0;
        return;
    }
    if (msg.isPitchWheel())
    {
        const float norm = (msg.getPitchWheelValue() - 8192) / 8192.0f;
        engine.setPitchBend (norm * pitchBendRangeSemis.load (std::memory_order_acquire));
        return;
    }
    if (msg.isController())
    {
        const int cc = msg.getControllerNumber(), val = msg.getControllerValue();
        if (cc == 64) { engine.setSustainPedal (val >= 64); return; }   // damper

        // A learned modifier CC (footswitch >=64) is consumed before MIDI-learn.
        bool held = false;
        const int mod = chordEngine.isEnabled() ? modifierLearn.handleCC (cc, val, held) : -1;
        if (mod >= 0)
        {
            if (held) midiModMask |= (1u << mod); else midiModMask &= ~(1u << mod);
            applyChordModifiers (qwertyModMask.load (std::memory_order_acquire) | midiModMask);
        }
        else
        {
            if (cc == 1) engine.setModWheel (val / 127.0f);            // mod wheel -> vibrato
            midiLearn.handleCC (msg.getChannel(), cc, val);
        }
    }
}

void VASynthProcessor::drainRoutedMidi (bool chordOn)
{
    int start1, size1, start2, size2;
    routedFifo.prepareToRead (routedFifo.getNumReady(), start1, size1, start2, size2);
    auto handle = [&] (int idx)
    {
        const auto& e = routedBuf[(std::size_t) idx];
        const juce::MidiMessage m (e.status, e.d1, e.d2);
        if (m.isNoteOn())       dispatchNoteOn  (e.d1, e.d2 / 127.0f, e.part, chordOn);
        else if (m.isNoteOff()) dispatchNoteOff (e.d1, e.part, chordOn);
        else                    handleControlMessage (m);
    };
    for (int i = 0; i < size1; ++i) handle (start1 + i);
    for (int i = 0; i < size2; ++i) handle (start2 + i);
    routedFifo.finishedRead (size1 + size2);
}

FXParams VASynthProcessor::snapshotFXParams() const
{
    namespace ID = ParamID;
    FXParams p;

    p.chorusRate  = rp (apvts, ID::chorusRate);
    p.chorusDepth = rp (apvts, ID::chorusDepth);
    p.chorusMix   = rp (apvts, ID::chorusMix);

    p.delayTimeMs   = rp (apvts, ID::delayTime);
    p.delayFeedback = rp (apvts, ID::delayFeedback);
    p.delayMix      = rp (apvts, ID::delayMix);
    p.delaySpread   = rp (apvts, ID::delaySpread);

    p.reverbSize  = rp (apvts, ID::reverbSize);
    p.reverbDamp  = rp (apvts, ID::reverbDamp);
    p.reverbWidth = rp (apvts, ID::reverbWidth);
    p.reverbMix   = rp (apvts, ID::reverbMix);

    p.width = rp (apvts, ID::stereoWidth);

    p.enabled[FXChain::Chorus_] = rp (apvts, ID::fxChorusOn) > 0.5f;
    p.enabled[FXChain::Delay_]  = rp (apvts, ID::fxDelayOn)  > 0.5f;
    p.enabled[FXChain::Reverb_] = rp (apvts, ID::fxReverbOn) > 0.5f;
    p.enabled[FXChain::Width_]  = rp (apvts, ID::fxWidthOn)  > 0.5f;

    getFxOrder (p.order);
    return p;
}

// Parse the persisted "a,b,c,d" fx_order property into the atomic mirror. Called
// after replaceState so a loaded session/preset restores its chain order. A
// missing or malformed property leaves the default 0,1,2,3.
void VASynthProcessor::applyFxOrderProperty()
{
    const auto str = apvts.state.getProperty (ParamID::fxOrder).toString();
    if (str.isEmpty()) return;
    auto tokens = juce::StringArray::fromTokens (str, ",", "");
    if (tokens.size() != 4) return;
    int order[4];
    for (int i = 0; i < 4; ++i) order[i] = tokens[i].getIntValue();
    setFxOrder (order);        // validates the permutation; ignores if malformed
}

void VASynthProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                     juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const auto tStart = std::chrono::steady_clock::now();   // cheap, RT-safe (vDSO)
    buffer.clear();

    const int numSamples = buffer.getNumSamples();
    // No resize on the audio thread — monoScratch was sized in prepareToPlay.
    // Guard against a misbehaving host sending an oversized block.
    jassert (numSamples <= monoScratch.getNumSamples());
    auto* mono = monoScratch.getWritePointer (0);
    juce::FloatVectorOperations::clear (mono, numSamples);

    // Merge QWERTY computer-keyboard notes into the MIDI stream (empty and free
    // when no keys are held), so they share the exact hardware-MIDI path below.
    qwertyKeyboardState.processNextMidiBuffer (midi, 0, numSamples, true);

    // Panic (RT-safe): a hot-unplug asked us to release everything.
    if (panicRequested.exchange (false, std::memory_order_acq_rel))
        { engine.allNotesOff(); chordEngine.reset(); midiModMask = 0; lastFedModMask = 0; }

    const auto params = snapshotParams();

    namespace ID = ParamID;
    const float lfoRate  = rp (apvts, ID::lfoRate);
    const float lfoDepth = rp (apvts, ID::lfoDepth);
    const int   lfoShape = (int) rp (apvts, ID::lfoShape);
    const int   lfoDest  = (int) rp (apvts, ID::lfoDest);
    const float master   = rp (apvts, ID::masterGain);

    // --- chord engine (7B): one played note -> a diatonic chord -------------
    const bool chordOn = rp (apvts, ID::chordEnabled) > 0.5f;
    chordEngine.setEnabled (chordOn);
    chordEngine.setRoot  ((int) rp (apvts, ID::chordRoot));
    chordEngine.setScale ((int) rp (apvts, ID::chordScale));
    // Chord mode FORCES poly — a chord in mono/legato would sound only one note.
    engine.setPolyMode (chordOn ? 0 : (int) rp (apvts, ID::polyMode));
    // Feed QWERTY modifier edges (message thread) into the latest-wins forcer stack.
    applyChordModifiers (qwertyModMask.load (std::memory_order_acquire) | midiModMask);

    // Routed surfaces (per-input MIDI / QWERTY-to-part) — drained at block start, so
    // they sound from sample 0 (block-granular; sample-accurate routing is future).
    drainRoutedMidi (chordOn);

    // --- sample-accurate MIDI dispatch --------------------------------------
    // Render up to each event, dispatch it, continue. This keeps note timing
    // tight even inside large buffers — important for anything sequenced.
    int renderedUpTo = 0;

    for (const auto metadata : midi)
    {
        const auto msg = metadata.getMessage();
        const int  pos = metadata.samplePosition;

        if (pos > renderedUpTo)
        {
            engine.render (mono + renderedUpTo, pos - renderedUpTo,
                           params, lfoRate, lfoShape, lfoDepth, lfoDest);
            renderedUpTo = pos;
        }

        if (msg.isNoteOn())                                 // host / QWERTY -> LIVE part 0
            dispatchNoteOn (msg.getNoteNumber(), msg.getFloatVelocity(), 0, chordOn);
        else if (msg.isNoteOff())
            dispatchNoteOff (msg.getNoteNumber(), 0, chordOn);
        else
            handleControlMessage (msg);                     // CC / pitch-bend / all-off
    }

    if (renderedUpTo < buffer.getNumSamples())
        engine.render (mono + renderedUpTo, buffer.getNumSamples() - renderedUpTo,
                       params, lfoRate, lfoShape, lfoDepth, lfoDest);

    // Publish the held-modifier mask (QWERTY | MIDI) for the CHORD UI indicators.
    activeModMask.store (qwertyModMask.load (std::memory_order_acquire) | midiModMask,
                         std::memory_order_release);

    // --- stereo FX chain: the engine sums to mono, so duplicate to L/R and run
    //     the reorderable chain (chorus/delay/reverb/width) in place. The chain
    //     is allocation-free; disabled blocks cost nothing.
    float* L = stereoScratch.getWritePointer (0);
    float* R = stereoScratch.getWritePointer (1);
    juce::FloatVectorOperations::copy (L, mono, numSamples);
    juce::FloatVectorOperations::copy (R, mono, numSamples);

    fxChain.setParams (snapshotFXParams());
    fxChain.process (L, R, numSamples);

    // --- master gain (per-sample ramp, kills zipper on gain steps/automation)
    //     followed by the safety soft-clipper — the LAST thing before output.
    //     The clipper is transparent (bit-exact) below its threshold, so normal
    //     playing is untouched; it only tames pathological dense-chord peaks so
    //     nothing ever hard-clips the DAC. Both are per-sample (the clip is
    //     nonlinear), and we count clipper engagements for telemetry.
    masterGain.setTargetValue (master);
    const bool smoothing = masterGain.isSmoothing();
    const float gConst = masterGain.getTargetValue();
    int clipSamples = 0;
    for (int i = 0; i < numSamples; ++i)
    {
        const float g = smoothing ? masterGain.getNextValue() : gConst;
        bool engaged = false;
        L[i] = SoftClip::process (L[i] * g, engaged);
        R[i] = SoftClip::process (R[i] * g, engaged);
        if (engaged) ++clipSamples;
    }
    health.logClip (clipSamples);

    // --- write to the output bus. Stereo: L/R; extra channels get L; a mono host
    //     gets the L+R average so nothing wet is lost.
    const int nCh = buffer.getNumChannels();
    if (nCh >= 2)
    {
        buffer.copyFrom (0, 0, L, numSamples);
        buffer.copyFrom (1, 0, R, numSamples);
        for (int ch = 2; ch < nCh; ++ch) buffer.copyFrom (ch, 0, L, numSamples);
    }
    else if (nCh == 1)
    {
        float* o = buffer.getWritePointer (0);
        for (int i = 0; i < numSamples; ++i) o[i] = 0.5f * (L[i] + R[i]);
    }

    // --- audio-health telemetry (RT-safe: pushes POD events only) -----------
    const auto tEnd = std::chrono::steady_clock::now();
    const float renderMs = (float) std::chrono::duration<double, std::milli> (tEnd - tStart).count();
    health.logRenderTime (renderMs, blockIndex);
    if (renderMs > (float) budgetMs)
        health.logOverrun (renderMs, (float) budgetMs, blockIndex);   // xrun early-warning
    health.logVoiceCount (engine.activeVoiceCount());
    const std::uint64_t steals = engine.stealCount();
    health.logSteals ((int) (steals - lastSteals));
    lastSteals = steals;
    ++blockIndex;
}

// --- state ---------------------------------------------------------------
void VASynthProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    midiLearn.saveToTree (state);                 // append MIDILEARN child
    modifierLearn.saveToTree (state);             // append MODIFIERLEARN child (7B)

    // Parts (7C): locked-part preset names + the surface routing table.
    state.removeChild (state.getChildWithName ("PARTS"), nullptr);
    juce::ValueTree parts ("PARTS");
    for (int p = 1; p < SynthEngine::maxParts; ++p)
        if (partPresetName[(std::size_t) p].isNotEmpty())
        {
            juce::ValueTree e ("PART");
            e.setProperty ("index", p, nullptr);
            e.setProperty ("preset", partPresetName[(std::size_t) p], nullptr);
            parts.addChild (e, -1, nullptr);
        }
    {
        const juce::ScopedLock sl (routingLock);
        for (auto& r : routingTable)
        {
            juce::ValueTree e ("ROUTE");
            e.setProperty ("surface", r.first, nullptr);
            e.setProperty ("part", r.second, nullptr);
            parts.addChild (e, -1, nullptr);
        }
    }
    state.addChild (parts, -1, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void VASynthProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        auto tree = juce::ValueTree::fromXml (*xml);

        // Detect pre-level-mixer states BEFORE replaceState back-fills the levels.
        const bool needsMigration = stateNeedsLevelMigration (tree);

        midiLearn.loadFromTree (tree);            // read MIDILEARN child (if any)
        modifierLearn.loadFromTree (tree);        // read MODIFIERLEARN child (7B)
        apvts.replaceState (tree);                // APVTS ignores the extra children

        // Old sessions (osc_mix, no osc1_level) derive the per-source levels from
        // the legacy crossfade so they sound the same.
        if (needsMigration) applyLegacyOscLevelMigration (apvts);

        // Restore the FX chain order (state-tree property, not an APVTS param).
        applyFxOrderProperty();

        // Parts (7C): re-bake locked-part presets (missing -> Init) + routing table.
        if (auto parts = tree.getChildWithName ("PARTS"); parts.isValid())
            for (auto e : parts)
            {
                if (e.hasType ("PART"))
                    setPartPreset ((int) e.getProperty ("index", -1), e.getProperty ("preset", juce::String()).toString());
                else if (e.hasType ("ROUTE"))
                    setSurfaceRouting (e.getProperty ("surface", juce::String()).toString(), (int) e.getProperty ("part", 0));
            }
    }
}

juce::AudioProcessorEditor* VASynthProcessor::createEditor()
{
    return new VASynthEditor (*this);
}

// JUCE entry point.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new VASynthProcessor();
}
