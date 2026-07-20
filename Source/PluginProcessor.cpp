#include "PluginProcessor.h"
#include "ModDestRegistry.h"
#include "PresetManager.h"     // randomizeExclusions() (shared policy for H5 RANDOM)
#include "PluginEditor.h"
#include "AppInfo.h"
#include "DSP/SoftClip.h"
#include <juce_audio_devices/juce_audio_devices.h>   // #85: juce::MidiOutput for the standalone clock send
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

static const juce::StringArray& perPartSoundIds();   // fwd (defined below) — the per-part sound params
static PartLfos lfosFrom (const juce::AudioProcessorValueTreeState& src);   // fwd (defined below)

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

    applyArpStepsProperty();      // seed the 16-step pattern with its default (all on)
    applySeqProperty();           // seed the sequencer grid (empty)
    macroTargetId = defaultMacroTargets();   // #55: factory macro assignments (M1..M8)
    writeMacroMapProperty();

    // Default scene, P1: the live part starts on a playable LEAD instead of a bare sine
    // so it is clearly distinct from the sequencer's drum part (P2). This is only the
    // BASELINE sound — the host / standalone restore of a saved patch (setStateInformation)
    // overrides it, so the player's own sound still persists across relaunches. Done before
    // the listeners register so it never flags the part "(edited)".
    if (factoryPresets.byName ("Bright Lead") != nullptr) loadFactoryPreset ("Bright Lead");

    // 1.3: mark a focused LOCKED part "(edited)" when the panel changes a sound param.
    for (auto& id : perPartSoundIds()) apvts.addParameterListener (id, this);
}

VASynthProcessor::~VASynthProcessor()
{
    for (auto& id : perPartSoundIds()) apvts.removeParameterListener (id, this);
}

void VASynthProcessor::parameterChanged (const juce::String&, float)
{
    if (loadingPartState) return;                       // ignore programmatic focus/preset loads
    const int f = editFocusPart.load (std::memory_order_relaxed);
    if (f > 0 && f < SynthEngine::maxParts) partEdited[(std::size_t) f] = true;
}

// Load a SOUND (a full scratch state tree) into the currently focused part WITHOUT
// disturbing any global performance state or the other parts. applyPartSoundFromTree
// copies only the per-part sound params (+ fx order) into the live APVTS — it never
// calls replaceState, so the sequencer pattern/target, looper, tempo, arp, chord,
// macros, mixer, EQ and master all stay exactly where the player left them. The
// focused part's stored state + engine slot are kept coherent so a later focus swap
// (or a MULTI save) sees the loaded sound.
void VASynthProcessor::applyFocusedPartSound (const juce::ValueTree& soundTree)
{
    applyPartSoundFromTree (soundTree);                    // sound params + fx order -> live slot
    const int f = editFocusPart.load (std::memory_order_relaxed);
    if (f >= 0 && f < SynthEngine::maxParts)
    {
        partEditState[(std::size_t) f] = apvts.copyState();
        partEdited[(std::size_t) f] = false;               // a freshly loaded preset is unedited
        if (f > 0)                                         // a LOCKED part must re-publish to keep sounding
            engine.setLockedPartParams (f, snapshotParams(), snapshotFXParams(), lfosFrom (apvts), partMatrix[(std::size_t) f]);
    }
}

void VASynthProcessor::loadFactoryPreset (const juce::String& name)
{
    const auto* p = factoryPresets.byName (name);
    if (p == nullptr) return;
    bool ok = true; juce::ValueTree st;
    bakePresetParams (name, ok, nullptr, nullptr, &st);    // preset -> scratch SOUND state
    applyFocusedPartSound (st);                            // sound-only: globals + other parts untouched
    // Factory FX order lives in the JSON (not the param set), so apply it explicitly.
    // Factory fxOrder is a 4-block permutation (pre-EQ); append EQ (4) as the last slot.
    int o[kFxCount] { 0, 1, 2, 3, 4 };
    if (p->fxOrder.size() == 4) for (int i = 0; i < 4; ++i) o[i] = p->fxOrder[i];
    setFxOrder (o);
}

void VASynthProcessor::loadUserPreset (const juce::String& name)
{
    bool ok = true; juce::ValueTree st;
    bakePresetParams (name, ok, nullptr, nullptr, &st);    // user *.vasynth -> scratch SOUND state (with its fx order)
    if (! ok) return;                                      // missing file -> leave everything as-is
    applyFocusedPartSound (st);                            // sound-only: globals + other parts untouched
}

void VASynthProcessor::loadInitPreset()
{
    // Init resets the focused part's SOUND to defaults but keeps ALL global performance
    // state (sequencer, looper, tempo, macros, mixer, master, ...) and the other parts put.
    applyFocusedPartSound (juce::ValueTree());             // invalid tree -> Init baseline sound
    const int def[kFxCount] { 0, 1, 2, 3, 4 };
    setFxOrder (def);
}

void VASynthProcessor::clearFocusedPartToBlank()
{
    namespace ID = ParamID;
    auto set01 = [this] (const char* id, float v) { if (auto* p = apvts.getParameter (id)) p->setValueNotifyingHost (v); };
    // Reset only the SELECTED part's sound-design params to default (the APVTS holds the
    // edit-focused part's sound; globals + other parts stay put), then force a clean sine.
    for (auto* rp : getParameters())
        if (auto* w = dynamic_cast<juce::AudioProcessorParameterWithID*> (rp))
            if (soundDesignParamIDs().contains (w->paramID))
                rp->setValueNotifyingHost (rp->getDefaultValue());
    set01 (ID::osc1On, 1.0f); set01 (ID::osc1Wave, 1.0f);          // osc1 sine (last of saw/sqr/tri/sin)
    set01 (ID::osc2On, 0.0f); set01 (ID::osc3On, 0.0f);            // single oscillator
    set01 (ID::noiseLevel, 0.0f);
    set01 (ID::fxChorusOn, 0.0f); set01 (ID::fxDelayOn, 0.0f);
    set01 (ID::fxReverbOn, 0.0f); set01 (ID::fxWidthOn, 0.0f);     // dry
}

juce::File VASynthProcessor::userMidiProfileDir()
{
    return AppInfo::midiProfileDir();
}

// Apply the matched profile's default CC map. Factory first, then user, so user
// A matched device profile is AUTHORITATIVE for the CCs it names: applying it FORCES those
// mappings, overriding any stale/learned binding (user profile still layers over factory).
// This is what makes "plug in a Launchkey -> its 8 pots drive the 8 macros" reliable even
// when an old session had learned those CCs onto something else. (Learn still wins live,
// until the next enumerate/hot-plug re-asserts the device's own map.)
void VASynthProcessor::applyDeviceProfile (const juce::String& deviceName)
{
    using Src = MidiLearnManager::Source;
    if (auto* fac = profileLib.factoryFor (deviceName))
    {
        for (auto& m : fac->mappings) midiLearn.applyProfileMapping (m.first, m.second, Src::Factory, /*force*/ true);
        pitchBendRangeSemis.store ((float) fac->pitchBendRange, std::memory_order_release);
    }
    if (auto* usr = profileLib.userFor (deviceName))
    {
        for (auto& m : usr->mappings) midiLearn.applyProfileMapping (m.first, m.second, Src::User, /*force*/ true);
        pitchBendRangeSemis.store ((float) usr->pitchBendRange, std::memory_order_release);
    }
}

// I1: the profile (user wins over factory) that gives this device a PAD sub-surface, or null.
const MidiProfile* VASynthProcessor::padProfileFor (const juce::String& deviceName) const
{
    if (auto* usr = profileLib.userFor (deviceName)) if (usr->hasPadSurface()) return usr;
    if (auto* fac = profileLib.factoryFor (deviceName)) if (fac->hasPadSurface()) return fac;
    return nullptr;
}

// The name of a device's pad sub-surface ("<device> Pads"), or empty if it has none. Used by
// the INPUTS dialog to list it and by routeDeviceMessage to tag its pad notes.
juce::String VASynthProcessor::padSubSurfaceName (const juce::String& deviceName) const
{
    return padProfileFor (deviceName) != nullptr ? deviceName + " Pads" : juce::String();
}

// The standalone's per-device MIDI entry point (I1). If this device has a pad sub-surface and
// the message is a note on/off on the pad channel within the pad note range, it is split off
// into the "<device> Pads" surface; everything else stays on the device's own surface. Keeps
// the "each message reaches exactly one surface" invariant the standalone relies on.
void VASynthProcessor::routeDeviceMessage (const juce::String& deviceName, const juce::MidiMessage& m)
{
    if (const auto* pad = padProfileFor (deviceName))
    {
        if (m.isNoteOnOrOff() && m.getChannel() == pad->padChannel
            && m.getNoteNumber() >= pad->padLo && m.getNoteNumber() <= pad->padHi)
        {
            routeSurfaceMessage (deviceName + " Pads", m);
            return;
        }
    }
    routeSurfaceMessage (deviceName, m);
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

// Max simultaneous voices. 24 (the full pool) — raised from 16 for the
// multitimbral split (sequencer kit + looper patch + live lead + spare all
// sounding at once). The voice-sum trim is DECOUPLED from the pool size
// (SynthEngine kTrimVoices = 16), so this bump never changes single-note level
// or the render goldens. CPU: the pathological worst case (all 24 held x 3 saws
// + all FX) hits ~109% of the derated ThinkPad block budget (would glitch);
// normal multitimbral use sits far lower. Reviewed and accepted (user, keep 24
// / accept the risk). Drop to 20 + an active-voice cap if live glitching shows up.
#ifndef VASYNTH_MAX_VOICES
 #define VASYNTH_MAX_VOICES 24
#endif

void VASynthProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.setOscQuality (VASYNTH_OSC_QUALITY);
    engine.setMaxVoices (VASYNTH_MAX_VOICES);
    engine.prepare (sampleRate, juce::jmax (1, samplesPerBlock));   // sizes the per-part FX buffers too
    // Allocate the stereo master buffer ONCE, at the host's max block size. processBlock
    // never resizes it (JUCE guarantees numSamples <= this). Per-part mix buffers + FX
    // live in the engine now (Sub-phase 2).
    stereoScratch.setSize (2, juce::jmax (1, samplesPerBlock), false, false, true);

    // Audio looper rings: ONE per part (4 lanes, #47), each sized by the documented
    // AudioLoop::kMaxLoopSeconds ceiling (24 s = 4 bars @ 40 BPM floor). Allocated once here;
    // the audio thread never resizes them (loop_bars/tempo only shorten the active length,
    // clamped to the ring). ~37 MB total @ 48 kHz — raise kMaxLoopSeconds for a studio build.
    for (auto& al : audioLoops) al.prepare (juce::jmax (1, (int) (sampleRate * AudioLoop::kMaxLoopSeconds)));

    masterGain.reset (sampleRate, 0.02);                       // ~20 ms ramp
    masterGain.setCurrentAndTargetValue (
        apvts.getRawParameterValue (ParamID::masterGain)->load());

    masterEQ.prepare (sampleRate);

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

    // Establish the default multitimbral layout ONCE (the engine is now prepared so parts
    // can bake). Parts 1-3 never persist across relaunch (routing lifecycle rule 2), so
    // this is the baseline layout every launch; an explicit MULTI load overrides it.
    if (! defaultSceneApplied) { applyDefaultScene(); defaultSceneApplied = true; }
}

// Default scene (startup layout). P1 (live) is seeded in the constructor with a lead; here
// the LOCKED parts get their dedicated sounds so the groove-box is playable out of the box
// and each surface is audibly distinct:
//   P4 (part 3) = 808 drum kit — the sequencer's DEFAULT target (seq_target = P4), so the
//                 sequencer plays drums, never the live lead.
//   P3 (part 2) = a bass patch — a second distinct locked voice (e.g. for the looper).
//   P2 (part 1) = left as Init (a spare part to split/assign freely).
// Only the LAYOUT is set here; no global (tempo/seq grid/macros) is touched.
void VASynthProcessor::applyDefaultScene()
{
    setPartKit (3, factoryKit ("808 Basics"));      // P4: sequencer's drum kit
    setPartPreset (2, "Fat Saw Bass");              // P3: dedicated bass voice
    // P2 (part 1) intentionally left at Init — a free spare part.
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
    p.polyMode  = (int) rp (apvts, ID::polyMode);   // per-part now: bakes with a locked part

    return p;
}

VoiceParams VASynthProcessor::snapshotParams() const { return buildVoiceParams (apvts); }

// The per-part SOUND parameters — everything buildVoiceParams / fxParamsFrom / lfosFrom
// read. Edit-focus swaps ONLY these between parts; global/performance params (master,
// mixer, tempo, arp, chord, macros, poly mode) stay put. (fx ORDER travels via its state
// property, handled alongside.) Scoping audit (task #50): this is the canonical per-part
// set — osc/filter/env/glide/vel + the whole FX chain (chorus/delay/reverb/width) + all
// three LFOs. The MASTER parametric EQ (eq_*) is the deliberate exception: it is a global
// finisher on the summed output; a per-part EQ block lives in each part's FX chain (task #51).
static const juce::StringArray& perPartSoundIds()
{
    namespace ID = ParamID;
    static const juce::StringArray ids {
        ID::osc1Wave, ID::osc2Wave, ID::osc3Wave, ID::osc1Octave, ID::osc2Octave, ID::osc3Octave,
        ID::osc1Detune, ID::osc2Detune, ID::osc3Detune, ID::osc1PW, ID::osc2PW, ID::osc3PW,
        ID::oscMix, ID::noiseLevel, ID::osc1Level, ID::osc2Level, ID::osc3Level, ID::osc1On, ID::osc2On, ID::osc3On,
        ID::velToAmp, ID::velToCutoff,
        ID::filterType, ID::filterCutoff, ID::filterReso, ID::filterEnvAmt, ID::filterKeytrack,
        ID::ampAttack, ID::ampDecay, ID::ampSustain, ID::ampRelease,
        ID::fltAttack, ID::fltDecay, ID::fltSustain, ID::fltRelease, ID::fltEnvToPitch, ID::glideTime,
        ID::lfoRate, ID::lfoDepth, ID::lfoShape, ID::lfoDest, ID::lfoSync, ID::lfoDiv,
        ID::lfo2Rate, ID::lfo2Depth, ID::lfo2Shape, ID::lfo2Dest, ID::lfo2Sync, ID::lfo2Div,
        ID::lfo3Rate, ID::lfo3Depth, ID::lfo3Shape, ID::lfo3Dest, ID::lfo3Sync, ID::lfo3Div,
        ID::chorusRate, ID::chorusDepth, ID::chorusMix, ID::fxChorusOn,
        ID::delayTime, ID::delayFeedback, ID::delayMix, ID::delaySpread, ID::fxDelayOn,
        ID::reverbSize, ID::reverbDamp, ID::reverbWidth, ID::reverbMix, ID::fxReverbOn,
        ID::stereoWidth, ID::fxWidthOn,
        ID::peqOn, ID::peqB1Freq, ID::peqB1Gain, ID::peqB1Q, ID::peqB1On,
        ID::peqB2Freq, ID::peqB2Gain, ID::peqB2Q, ID::peqB2On,
        ID::peqB3Freq, ID::peqB3Gain, ID::peqB3Q, ID::peqB3On,
        ID::peqB4Freq, ID::peqB4Gain, ID::peqB4Q, ID::peqB4On,
        ID::peqB5Freq, ID::peqB5Gain, ID::peqB5Q, ID::peqB5On,
        ID::polyMode        // per-part Poly/Mono/Legato (edited via focus like the rest of the sound)
    };
    return ids;
}

const juce::StringArray& VASynthProcessor::soundDesignParamIDs() { return perPartSoundIds(); }

void VASynthProcessor::flushLoopNotes (bool chordOn)
{
    for (int pt = 0; pt < SynthEngine::maxParts; ++pt) flushLoopNotesForPart (pt, chordOn);
}

// Release notes ONE lane's MIDI playback left sounding (per-lane stop/clear, #47).
void VASynthProcessor::flushLoopNotesForPart (int pt, bool chordOn)
{
    if (pt < 0 || pt >= SynthEngine::maxParts) return;
    for (int n = 0; n < 128; ++n)
        if (loopNoteHeld[(std::size_t) pt][(std::size_t) n])
        { dispatchNoteOff (n, pt, chordOn); loopNoteHeld[(std::size_t) pt][(std::size_t) n] = false; }
}

// ============================================================================
// J3 scenes. The ACTIVE scene IS the live state (looper clips + drum pattern + per-lane
// transport). Tapping a scene arms it (pendingScene); it engages at the next launch-quantum
// boundary. Long-press posts a clone/clear command. All scene mutation happens on the audio
// thread; only the transport-param restore is bounced to the message thread (AsyncUpdater),
// since setValueNotifyingHost is not RT-safe.
// ============================================================================
namespace
{
    // Per-lane transport param IDs (lane N == part N; lane 1 keeps the original IDs).
    const char* const kBarsIds[]  { ParamID::loopBars,  ParamID::loopBars2, ParamID::loopBars3, ParamID::loopBars4 };
    const char* const kModeIds[]  { ParamID::loopMode,  ParamID::loopMode2, ParamID::loopMode3, ParamID::loopMode4 };
    const char* const kPlayIds[]  { ParamID::loopPlay,  ParamID::loopPlay2, ParamID::loopPlay3, ParamID::loopPlay4 };
    const char* const kQuantIds[] { ParamID::loopQuant, ParamID::loopQuant2, ParamID::loopQuant3, ParamID::loopQuant4 };
}

void VASynthProcessor::launchScene (int i)
{
    if (i < 0 || i >= kScenes) return;
    // Re-tapping the pending scene, or tapping the already-active one, cancels the pending switch.
    const int cur = pendingScene.load (std::memory_order_relaxed);
    const int pend = (i == cur || i == activeSceneDisp.load (std::memory_order_relaxed)) ? -1 : i;
    pendingScene.store (pend, std::memory_order_release);
    pendingSceneDisp.store (pend, std::memory_order_relaxed);
}

void VASynthProcessor::copyActiveSceneTo (int i) { if (i >= 0 && i < kScenes) sceneCopyCmd.store (i, std::memory_order_release); }
void VASynthProcessor::clearScene       (int i) { if (i >= 0 && i < kScenes) sceneClearCmd.store (i, std::memory_order_release); }

bool VASynthProcessor::liveHasContent() const
{
    for (int p = 0; p < SynthEngine::maxParts; ++p)
        if (looper.hasContent (p) || audioLoops[(std::size_t) p].hasContent()) return true;
    for (auto& row : seqCells) for (unsigned char c : row) if (c) return true;
    return false;
}

void VASynthProcessor::captureLiveTransportInto (SceneSlot& s) const
{
    for (int L = 0; L < SynthEngine::maxParts; ++L)
    {
        s.tBars[(std::size_t) L]  = rp (apvts, kBarsIds[L]);
        s.tMode[(std::size_t) L]  = rp (apvts, kModeIds[L]);
        s.tPlay[(std::size_t) L]  = rp (apvts, kPlayIds[L]);
        s.tQuant[(std::size_t) L] = rp (apvts, kQuantIds[L]);
    }
}

// Message thread: push the just-engaged scene's transport values into the APVTS params so the UI
// and the DSP agree. A one-message-cycle lag behind the (sample-accurate) clip/pattern flip.
void VASynthProcessor::handleAsyncUpdate()
{
    // 1) Restore the just-engaged scene's transport params into the APVTS (flip only).
    const int to = restoreTransportScene.exchange (-1, std::memory_order_acquire);
    if (to >= 0 && to < kScenes)
    {
        const auto& s = scenes[(std::size_t) to];
        auto set = [this] (const char* id, float norm)
        { if (auto* p = apvts.getParameter (id)) p->setValueNotifyingHost (p->convertTo0to1 (norm)); };
        for (int L = 0; L < SynthEngine::maxParts; ++L)
        {
            set (kBarsIds[L],  s.tBars[(std::size_t) L]);
            set (kModeIds[L],  s.tMode[(std::size_t) L]);
            set (kPlayIds[L],  s.tPlay[(std::size_t) L]);
            set (kQuantIds[L], s.tQuant[(std::size_t) L]);
        }
    }

    // 2) All scene-AUDIO heap work happens here on the message thread: flip swap, copy-here, clear.
    doSceneAudioSwap();

    const int copyTo = pendingAudioCopyToScene.exchange (-1, std::memory_order_acquire);
    if (copyTo >= 0 && copyTo < kScenes)                 // clone the live audio into a slot
        for (int L = 0; L < SynthEngine::maxParts; ++L)
        {
            const std::size_t oldB = sceneAudioLaneBytes (copyTo, L);
            audioLoops[(std::size_t) L].snapshotInto (sceneAudioL[(std::size_t) copyTo][(std::size_t) L],
                                                      sceneAudioR[(std::size_t) copyTo][(std::size_t) L]);
            std::size_t total = sceneAudioBytes.load (std::memory_order_relaxed) - oldB + sceneAudioLaneBytes (copyTo, L);
            if (total > kMaxSceneAudioBytes)
            { sceneAudioL[(std::size_t) copyTo][(std::size_t) L] = {}; sceneAudioR[(std::size_t) copyTo][(std::size_t) L] = {};
              total = sceneAudioBytes.load (std::memory_order_relaxed) - oldB; postToast ("Scene memory full - audio not saved"); }
            sceneAudioBytes.store (total, std::memory_order_relaxed);
        }

    const int clr = pendingAudioClearScene.exchange (-1, std::memory_order_acquire);
    if (clr >= 0 && clr < kScenes)                       // free a wiped slot's audio
        for (int L = 0; L < SynthEngine::maxParts; ++L)
        {
            sceneAudioBytes.fetch_sub (sceneAudioLaneBytes (clr, L), std::memory_order_relaxed);
            sceneAudioL[(std::size_t) clr][(std::size_t) L] = {};
            sceneAudioR[(std::size_t) clr][(std::size_t) L] = {};
        }
}

std::size_t VASynthProcessor::sceneAudioLaneBytes (int scene, int lane) const
{
    return (sceneAudioL[(std::size_t) scene][(std::size_t) lane].size()
          + sceneAudioR[(std::size_t) scene][(std::size_t) lane].size()) * sizeof (float);
}

// Message thread: snapshot the current live AUDIO loop into a scene's heap buffer, tracking a
// running memory total and refusing (loudly) past the cap rather than growing without bound.
static void snapshotAudioLane (AudioLoop& al, std::vector<float>& dstL, std::vector<float>& dstR) { al.snapshotInto (dstL, dstR); }

// Message thread: snapshot the outgoing scene's live AUDIO into its heap buffers and recall the
// incoming scene's audio into the live rings. Each lane is guarded (the audio thread skips it) so
// there is no race while we overwrite the ring. Compact: only the recorded region is held.
void VASynthProcessor::doSceneAudioSwap()
{
    const int from = pendingAudioSwapFrom.exchange (-1, std::memory_order_acquire);
    const int to   = pendingAudioSwapTo.exchange   (-1, std::memory_order_acquire);
    if (from < 0 || to < 0 || from >= kScenes || to >= kScenes) return;

    for (int L = 0; L < SynthEngine::maxParts; ++L)
    {
        auto& al = audioLoops[(std::size_t) L];
        const std::size_t oldBytes = sceneAudioLaneBytes (from, L);
        snapshotAudioLane (al, sceneAudioL[(std::size_t) from][(std::size_t) L], sceneAudioR[(std::size_t) from][(std::size_t) L]);
        std::size_t total = sceneAudioBytes.load (std::memory_order_relaxed) - oldBytes + sceneAudioLaneBytes (from, L);
        if (total > kMaxSceneAudioBytes)   // over budget: drop this snapshot, warn loudly
        {
            sceneAudioL[(std::size_t) from][(std::size_t) L] = {};
            sceneAudioR[(std::size_t) from][(std::size_t) L] = {};
            total = sceneAudioBytes.load (std::memory_order_relaxed) - oldBytes;
            postToast ("Scene memory full - audio not saved");
        }
        sceneAudioBytes.store (total, std::memory_order_relaxed);

        al.loadFrom (sceneAudioL[(std::size_t) to][(std::size_t) L], sceneAudioR[(std::size_t) to][(std::size_t) L]);
        audioSwapGuard[(std::size_t) L].store (false, std::memory_order_release);   // ring is safe again
    }
}

// Audio thread: at a launch boundary, swap the live clips + drum pattern with scene `to`, saving
// the outgoing live state into `from`. Recording lanes are left untouched so an in-flight take
// completes into the incoming scene (the edge rule). Flushes held loop notes so it starts clean.
void VASynthProcessor::engageSceneFlip (int from, int to, bool chordOn)
{
    if (from < 0 || from >= kScenes || to < 0 || to >= kScenes || from == to) return;
    auto& S = scenes[(std::size_t) from];
    auto& D = scenes[(std::size_t) to];

    for (int L = 0; L < SynthEngine::maxParts; ++L)
    {
        if (loopRecording[(std::size_t) L]) continue;    // defensive: a switch never engages mid-record
        S.lanes[(std::size_t) L] = looper.laneContent (L);          // save outgoing clip
        looper.setLaneContent (L, D.lanes[(std::size_t) L]);        // load incoming clip
        flushLoopNotesForPart (L, chordOn);                         // release anything the old clip held
    }
    // drum pattern (whole; the sequencer re-reads seqCells via setConfig next block)
    S.cells = seqCells; S.vel = seqVel; S.notes = seqNotes; S.mutes = seqMutes;
    seqCells = D.cells; seqVel = D.vel; seqNotes = D.notes; seqMutes = D.mutes;
    // transport: snapshot outgoing live params, request the incoming ones be restored (message thread)
    captureLiveTransportInto (S);
    S.has = liveHasContent();

    // J3 feedback: a newly-activated scene starts from the BEGINNING — rewind the loop transport to
    // its downbeat and re-anchor the drum sequencer, so nothing resumes mid-phrase.
    looper.rewind();
    sceneLastBar = 0; sceneClockPrimed = false;   // re-prime the launch-quantum bar tracker at the new origin
    if (seq.enabled()) seq.realign();

    activeSceneAudio = to;
    activeSceneDisp.store (to, std::memory_order_relaxed);
    pendingScene.store (-1, std::memory_order_relaxed);
    pendingSceneDisp.store (-1, std::memory_order_relaxed);
    sceneContentDisp[(std::size_t) from].store (S.has, std::memory_order_relaxed);

    // Audio loops are heavy to copy, so the swap runs off the audio thread (below). Guard every
    // lane now so the audio looper block skips it (brief silence) until the swap completes.
    for (int L = 0; L < SynthEngine::maxParts; ++L) audioSwapGuard[(std::size_t) L].store (true, std::memory_order_release);
    pendingAudioSwapFrom.store (from, std::memory_order_relaxed);
    pendingAudioSwapTo.store   (to,   std::memory_order_release);

    restoreTransportScene.store (to, std::memory_order_release);
    triggerAsyncUpdate();
}

// Audio thread: apply pending long-press menu commands (clone active scene into a slot / clear a slot).
void VASynthProcessor::serviceSceneCommands (bool chordOn)
{
    const int copyTo = sceneCopyCmd.exchange (-1, std::memory_order_acquire);
    if (copyTo >= 0 && copyTo < kScenes && copyTo != activeSceneAudio)
    {
        auto& D = scenes[(std::size_t) copyTo];
        for (int L = 0; L < SynthEngine::maxParts; ++L) D.lanes[(std::size_t) L] = looper.laneContent (L);
        D.cells = seqCells; D.vel = seqVel; D.notes = seqNotes; D.mutes = seqMutes;
        captureLiveTransportInto (D);
        D.has = liveHasContent();
        sceneContentDisp[(std::size_t) copyTo].store (D.has, std::memory_order_relaxed);
        pendingAudioCopyToScene.store (copyTo, std::memory_order_release);   // audio: cloned on the message thread
        triggerAsyncUpdate();
    }

    const int clr = sceneClearCmd.exchange (-1, std::memory_order_acquire);
    if (clr >= 0 && clr < kScenes)
    {
        if (clr == activeSceneAudio)   // clearing the live scene wipes the live clips + pattern now
        {
            for (int L = 0; L < SynthEngine::maxParts; ++L) { looper.clear (L); audioLoops[(std::size_t) L].clear(); flushLoopNotesForPart (L, chordOn); }
            seqCells = {}; seqVel = {}; for (auto& m : seqMutes) m = false;
        }
        scenes[(std::size_t) clr] = SceneSlot{};   // reset to a blank canvas (SceneSlot is heap-free -> RT-safe)
        sceneContentDisp[(std::size_t) clr].store (false, std::memory_order_relaxed);
        pendingAudioClearScene.store (clr, std::memory_order_release);       // audio: freed on the message thread
        triggerAsyncUpdate();
    }
}

void VASynthProcessor::applyChordModifiers (std::uint32_t combined)
{
    const std::uint32_t changed = combined ^ lastFedModMask;
    if (changed == 0) return;
    for (int i = 0; i < ChordEngine::kNumModifiers; ++i)
        if ((changed >> i) & 1u)
            chordEngine.setModifierHeld (i, (combined >> i) & 1u);
    lastFedModMask = combined;

    // 1.4: a modifier edge RE-VOICES every held chord on the LIVE/focused part — release
    // the tones that dropped out, trigger the ones that came in (velocity inherited);
    // unchanged tones keep sounding. So holding a chord and tapping MIN->MAJ7 morphs it.
    if (chordEngine.isEnabled())
    {
        const int part = playFocusPart.load (std::memory_order_relaxed);
        chordEngine.revoiceHeld ([this, part] (int n)          { engine.noteOff (n, part); },
                                 [this, part] (int n, float v) { engine.noteOn  (n, v, part); });
    }
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

// Read a full FX config (params + enables + chain order) from any APVTS — used to bake a
// locked part's FX from its source preset, so the part sounds like loading it live.
static FXParams fxParamsFrom (const juce::AudioProcessorValueTreeState& src)
{
    namespace ID = ParamID; FXParams p;
    p.chorusRate = rp (src, ID::chorusRate); p.chorusDepth = rp (src, ID::chorusDepth); p.chorusMix = rp (src, ID::chorusMix);
    p.delayTimeMs = rp (src, ID::delayTime); p.delayFeedback = rp (src, ID::delayFeedback);
    p.delayMix = rp (src, ID::delayMix);     p.delaySpread = rp (src, ID::delaySpread);
    p.reverbSize = rp (src, ID::reverbSize); p.reverbDamp = rp (src, ID::reverbDamp);
    p.reverbWidth = rp (src, ID::reverbWidth); p.reverbMix = rp (src, ID::reverbMix);
    p.width = rp (src, ID::stereoWidth);
    p.eqBand1 = { rp (src, ID::peqB1Freq), rp (src, ID::peqB1Gain), rp (src, ID::peqB1Q), rp (src, ID::peqB1On) > 0.5f };
    p.eqBand2 = { rp (src, ID::peqB2Freq), rp (src, ID::peqB2Gain), rp (src, ID::peqB2Q), rp (src, ID::peqB2On) > 0.5f };
    p.eqBand3 = { rp (src, ID::peqB3Freq), rp (src, ID::peqB3Gain), rp (src, ID::peqB3Q), rp (src, ID::peqB3On) > 0.5f };
    p.eqBand4 = { rp (src, ID::peqB4Freq), rp (src, ID::peqB4Gain), rp (src, ID::peqB4Q), rp (src, ID::peqB4On) > 0.5f };
    p.eqBand5 = { rp (src, ID::peqB5Freq), rp (src, ID::peqB5Gain), rp (src, ID::peqB5Q), rp (src, ID::peqB5On) > 0.5f };
    p.enabled[FXChain::Chorus_] = rp (src, ID::fxChorusOn) > 0.5f;
    p.enabled[FXChain::Delay_]  = rp (src, ID::fxDelayOn)  > 0.5f;
    p.enabled[FXChain::Reverb_] = rp (src, ID::fxReverbOn) > 0.5f;
    p.enabled[FXChain::Width_]  = rp (src, ID::fxWidthOn)  > 0.5f;
    p.enabled[FXChain::EQ_]     = rp (src, ID::peqOn)      > 0.5f;
    // Order (permutation of 0..kNumFX-1). Accept the current 5-slot form; if a legacy
    // 4-slot order is read (pre-EQ presets), keep it and append EQ (4) last.
    auto toks = juce::StringArray::fromTokens (src.state.getProperty (ID::fxOrder).toString(), ",", "");
    const int n = FXChain::kNumFX;
    if (toks.size() >= 4 && toks.size() <= n)
    {
        int o[FXChain::kNumFX]; bool seen[FXChain::kNumFX] = {}; bool okOrder = true;
        for (int i = 0; i < toks.size(); ++i)
        { o[i] = toks[i].getIntValue(); if (o[i] < 0 || o[i] >= n || seen[o[i]]) { okOrder = false; break; } seen[o[i]] = true; }
        // Fill any slots the legacy order didn't mention (e.g. EQ) with the remaining ids.
        for (int i = toks.size(); okOrder && i < n; ++i)
            for (int v = 0; v < n; ++v) if (! seen[v]) { o[i] = v; seen[v] = true; break; }
        if (okOrder) for (int i = 0; i < n; ++i) p.order[i] = o[i];
    }
    return p;
}

// Read the three LFOs from any APVTS (for baking a locked part's modulation).
static PartLfos lfosFrom (const juce::AudioProcessorValueTreeState& src)
{
    namespace ID = ParamID; PartLfos pl;
    pl.lfo[0] = { rp (src, ID::lfoRate),  rp (src, ID::lfoDepth),  (int) rp (src, ID::lfoShape),  (int) rp (src, ID::lfoDest),  rp (src, ID::lfoSync)  > 0.5f, (int) rp (src, ID::lfoDiv) };
    pl.lfo[1] = { rp (src, ID::lfo2Rate), rp (src, ID::lfo2Depth), (int) rp (src, ID::lfo2Shape), (int) rp (src, ID::lfo2Dest), rp (src, ID::lfo2Sync) > 0.5f, (int) rp (src, ID::lfo2Div) };
    pl.lfo[2] = { rp (src, ID::lfo3Rate), rp (src, ID::lfo3Depth), (int) rp (src, ID::lfo3Shape), (int) rp (src, ID::lfo3Dest), rp (src, ID::lfo3Sync) > 0.5f, (int) rp (src, ID::lfo3Div) };
    return pl;
}

// Bake a source preset (factory / user / Init) into VoiceParams via the scratch APVTS,
// reusing the exact kill-fold so a baked part is bit-identical to loading it live.
// `ok` is false if a named preset was missing (Init baked instead). Optionally also
// extracts the preset's FX + LFO (for a locked part; kit pads pass null). Shared by
// locked parts and kit pads.
VoiceParams VASynthProcessor::bakePresetParams (const juce::String& name, bool& ok,
                                                FXParams* fxOut, PartLfos* lfoOut,
                                                juce::ValueTree* stateOut)
{
    BakeProcessor scratch;
    ok = true;
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
        ok = false;                                 // missing preset -> Init fallback
        bakeInitBaseline (scratch);
        juce::Logger::writeToLog ("preset '" + name + "' missing -> Init");
    }
    if (fxOut    != nullptr) *fxOut    = fxParamsFrom (scratch.apvts);
    if (lfoOut   != nullptr) *lfoOut   = lfosFrom (scratch.apvts);
    if (stateOut != nullptr) *stateOut = scratch.apvts.copyState();   // full panel state (1.3 edit-focus)
    return buildVoiceParams (scratch.apvts);
}

VoiceParams VASynthProcessor::bakeVoiceStateParams (const juce::ValueTree& state)
{
    BakeProcessor scratch;
    if (state.isValid()) scratch.apvts.replaceState (state);          // an edited pad voice
    else                 bakeInitBaseline (scratch);
    return buildVoiceParams (scratch.apvts);
}

// Bake an arbitrary panel state tree into a part's engine slot (1.3): the part plays
// this (possibly edited) sound while it's not the focused/live part.
void VASynthProcessor::bakeStateToSlot (int part, const juce::ValueTree& state)
{
    if (part < 0 || part >= SynthEngine::maxParts) return;
    BakeProcessor scratch;
    if (state.isValid()) scratch.apvts.replaceState (state.createCopy());
    else                 bakeInitBaseline (scratch);
    engine.setLockedPartParams (part, buildVoiceParams (scratch.apvts),
                                fxParamsFrom (scratch.apvts), lfosFrom (scratch.apvts),
                                partMatrix[(std::size_t) juce::jlimit (0, SynthEngine::maxParts - 1, part)]);
}

// Copy ONLY the per-part sound params (+ fx order) from a stored state tree into the live
// APVTS, leaving global/performance params untouched. The panel refreshes via attachments.
void VASynthProcessor::applyPartSoundFromTree (const juce::ValueTree& tree)
{
    const juce::ScopedValueSetter<bool> guard (loadingPartState, true);   // don't flag "(edited)"
    BakeProcessor scratch;
    if (tree.isValid()) scratch.apvts.replaceState (tree.createCopy());
    else                bakeInitBaseline (scratch);
    for (auto& id : perPartSoundIds())
        if (auto* dst = apvts.getParameter (id))
            if (auto* src = scratch.apvts.getParameter (id))
                dst->setValueNotifyingHost (src->getValue());
    apvts.state.setProperty (ParamID::fxOrder, scratch.apvts.state.getProperty (ParamID::fxOrder), nullptr);
    applyFxOrderProperty();
}

// Move the edit focus (1.3). Save the current part's sound + bake it into its slot so it
// keeps sounding; load the tapped part's sound into the APVTS (panel refreshes). Only the
// per-part SOUND params move — the mixer, tempo, arp, chord, macros, master stay put.
void VASynthProcessor::setEditFocus (int part)
{
    if (part < 0 || part >= SynthEngine::maxParts) return;

    // The LIVE keyboard always follows the tap (a kit part triggers its pads; a synth part
    // plays its sound). The audio thread does the note hand-off when play-focus changes.
    playFocusPart.store (part, std::memory_order_release);

    // K2: a KIT part CAN be edit-focused now — its per-part channel EQ (and level/pan)
    // follow the tap. The kit has no single synth sound, so the osc/filter/env/LFO panels
    // show as inactive (dimmed + "edit pads in Kit Editor") while focused; the kit still
    // plays from its per-pad voices (they ignore the live synth params). The EQ reads/writes
    // this part's stored peq, preserved per part via partEditState below.
    const int cur = editFocusPart.load (std::memory_order_relaxed);
    if (part == cur) return;

    partEditState[(std::size_t) cur] = apvts.copyState();                    // remember cur's sound
    engine.setLockedPartParams (cur, snapshotParams(), snapshotFXParams(), lfosFrom (apvts), partMatrix[(std::size_t) cur]);  // keep it playing

    if (! partEditState[(std::size_t) part].isValid())                       // never visited -> Init
    { BakeProcessor b; bakeInitBaseline (b); partEditState[(std::size_t) part] = b.apvts.copyState(); }

    applyPartSoundFromTree (partEditState[(std::size_t) part]);              // panel swaps to this part
    editFocusPart.store (part, std::memory_order_release);                   // audio thread re-primes smoothing
}

void VASynthProcessor::revertPartToPreset (int part)
{
    if (part < 1 || part >= SynthEngine::maxParts) return;
    const auto name = partPresetName[(std::size_t) part];
    if (name.isEmpty()) return;
    setPartPreset (part, name);                        // re-bakes + repopulates the edit state, clears edited
}

bool VASynthProcessor::beginKitPadEdit (int part, int pad)
{
    if (part < 1 || part >= SynthEngine::maxParts || pad < 0 || pad >= kMaxKitPads) return false;
    if (isEditingKitPad() || ! isPartKit (part)) return false;
    auto& pd = partKits[(std::size_t) part].pads[(std::size_t) pad];
    if (pd.triggerNote < 0) return false;              // empty pad has no voice to edit

    // Seed the pad's voice from its source preset the first time it's edited.
    if (! pd.voiceState.isValid())
    { bool ok = true; juce::ValueTree st; bakePresetParams (pd.source, ok, nullptr, nullptr, &st); pd.voiceState = st; }

    // Save the current focus + its sound, then make this part a LIVE synth showing the pad
    // voice: the panel edits it and any played note auditions it.
    kitPadEditPart = part; kitPadEditPad = pad;
    kitPadEditSavedFocus = editFocusPart.load (std::memory_order_relaxed);
    partEditState[(std::size_t) kitPadEditSavedFocus] = apvts.copyState();
    engine.releasePartNotes (kitPadEditSavedFocus);
    // Keep the kit intact and route ONLY this pad through the live panel params: the other
    // pads keep their baked sound (the sequencer plays the full kit normally) while the
    // panel edits + auditions just this one.
    engine.setLiveKitPad (part, pad);
    applyPartSoundFromTree (pd.voiceState);            // panel shows the pad voice
    editFocusPart.store (part, std::memory_order_release);
    playFocusPart.store (part, std::memory_order_release);
    kitPadEditActive.store (true, std::memory_order_release);
    return true;
}

void VASynthProcessor::endKitPadEdit (bool commit)
{
    if (! isEditingKitPad()) return;
    const int part = kitPadEditPart, pad = kitPadEditPad, saved = kitPadEditSavedFocus;
    if (commit)
        partKits[(std::size_t) part].pads[(std::size_t) pad].voiceState = apvts.copyState();

    kitPadEditActive.store (false, std::memory_order_release);
    engine.setLiveKitPad (-1, -1);                     // stop routing the pad through live params
    engine.releasePartNotes (part);
    setPartKit (part, partKits[(std::size_t) part]);   // re-bake the kit with the edited pad
    // Restore the previously focused part + its sound.
    applyPartSoundFromTree (partEditState[(std::size_t) saved]);
    editFocusPart.store (saved, std::memory_order_release);
    playFocusPart.store (saved, std::memory_order_release);
}

bool VASynthProcessor::setPartPreset (int part, const juce::String& name)
{
    if (part < 1 || part >= SynthEngine::maxParts) return false;

    bool ok = true;
    FXParams fx; PartLfos lfo; juce::ValueTree st;
    const VoiceParams vp = bakePresetParams (name, ok, &fx, &lfo, &st);
    engine.setLockedPartParams (part, vp, fx, lfo, partMatrix[(std::size_t) part]);  // voice + FX + LFOs + matrix
    engine.clearPartKit (part);                     // a plain preset turns any kit off
    partKits[(std::size_t) part] = KitDefinition{}; // forget the kit definition
    partPresetName[(std::size_t) part] = ok ? name : juce::String ("Init");

    // 1.3 edit-focus: the preset IS this part's editable panel state now (fresh, unedited).
    partEditState[(std::size_t) part] = st;
    partEdited[(std::size_t) part] = false;
    if (editFocusPart.load (std::memory_order_relaxed) == part)     // focused -> refresh the panel
        applyPartSoundFromTree (st);
    return ok;
}

void VASynthProcessor::setPartKit (int part, const KitDefinition& def)
{
    if (part < 1 || part >= SynthEngine::maxParts) return;

    KitData kd; kd.isKit = true;
    for (int i = 0; i < kMaxKitPads; ++i)
    {
        const auto& pd = def.pads[(std::size_t) i];
        auto& out = kd.pads[(std::size_t) i];
        if (pd.triggerNote < 0) { out.triggerNote = -1; continue; }   // empty pad

        out.triggerNote = pd.triggerNote;
        out.numSound    = juce::jlimit (1, kMaxPadSoundNotes, pd.numSound);
        for (int s = 0; s < kMaxPadSoundNotes; ++s) out.soundNote[(std::size_t) s] = pd.soundNote[(std::size_t) s];
        out.chokeGroup  = juce::jlimit (0, 8, pd.chokeGroup);

        // I2: a SAMPLE pad resolves its managed buffer and carries borrowed pointers into the
        // engine POD; the synth VoiceParams path is skipped. Play-as-recorded: the sample root =
        // the pad's sounding note, so ratio == 1.0 by default (no accidental repitch). A missing/
        // unresolvable sample falls back to a silent pad (logged), never a crash.
        if (pd.samplePath.isNotEmpty())
        {
            if (const SampleData* sd = sampleStore.resolve (pd.samplePath))
            {
                out.isSample   = true;
                out.sampleL    = sd->L.data();
                out.sampleR    = sd->R.data();
                out.sampleLen  = sd->len;
                out.sampleSR   = sd->nativeSR;
                out.sampleRoot = out.soundNote[0];
                out.sampleGain = juce::jlimit (0.0f, 4.0f, pd.level);
                continue;                                                // no synth VoiceParams for a sample pad
            }
            // A sample pad whose file can't be resolved is SILENT (not a surprise synth sound).
            juce::Logger::writeToLog ("kit pad " + juce::String (i) + ": sample '" + pd.samplePath + "' missing -> silent");
            out.triggerNote = -1;
            continue;
        }

        bool ok = true;
        // Edited pads (Group 4 kit editing) bake from their own voice state; otherwise from
        // the source preset. Either way it's a full VoiceParams built the same kill-fold way.
        auto vp = pd.voiceState.isValid() ? bakeVoiceStateParams (pd.voiceState)
                                          : bakePresetParams (pd.source, ok);
        vp.gain *= juce::jlimit (0.0f, 4.0f, pd.level);               // fold pad level into the voice
        kd.params[(std::size_t) i] = vp;
    }
    engine.setPartKit (part, kd);                    // a Kit part is dry in v1 (engine uses dry FX/LFO for kits)
    partKits[(std::size_t) part] = def;
    partPresetName[(std::size_t) part] = def.name.isNotEmpty() ? def.name : juce::String ("Kit");
}

bool VASynthProcessor::importPadSample (int part, int pad, const juce::File& file)
{
    if (part < 1 || part >= SynthEngine::maxParts || pad < 0 || pad >= kMaxKitPads) return false;
    const juce::String key = sampleStore.importFile (file);
    if (key.isEmpty()) return false;
    auto def = partKits[(std::size_t) part];
    auto& pd = def.pads[(std::size_t) pad];
    if (pd.triggerNote < 0) pd.triggerNote = 36 + pad;   // seed an empty pad so it can sound
    pd.samplePath = key;
    pd.voiceState = {};                                  // a sample supersedes any edited synth voice
    setPartKit (part, def);                              // re-bake + publish
    return true;
}

void VASynthProcessor::clearPadSample (int part, int pad)
{
    if (part < 1 || part >= SynthEngine::maxParts || pad < 0 || pad >= kMaxKitPads) return;
    auto def = partKits[(std::size_t) part];
    def.pads[(std::size_t) pad].samplePath = {};
    setPartKit (part, def);
}

// ---- kit serialisation (shared by *.kit files and MULTI) --------------------

juce::ValueTree VASynthProcessor::kitToTree (const KitDefinition& def)
{
    juce::ValueTree t ("KIT");
    t.setProperty ("name", def.name, nullptr);
    for (int i = 0; i < kMaxKitPads; ++i)
    {
        const auto& pd = def.pads[(std::size_t) i];
        if (pd.triggerNote < 0) continue;                    // skip empty pads
        juce::ValueTree p ("PAD");
        p.setProperty ("trigger", pd.triggerNote, nullptr);
        p.setProperty ("source",  pd.source, nullptr);
        if (pd.samplePath.isNotEmpty()) p.setProperty ("sample", pd.samplePath, nullptr);   // I2: md5 key
        p.setProperty ("num",     pd.numSound, nullptr);
        p.setProperty ("level",   pd.level, nullptr);
        p.setProperty ("choke",   pd.chokeGroup, nullptr);
        for (int s = 0; s < 4; ++s) p.setProperty ("n" + juce::String (s), pd.soundNote[(std::size_t) s], nullptr);
        if (pd.voiceState.isValid()) p.addChild (pd.voiceState.createCopy(), -1, nullptr);   // edited pad voice
        t.addChild (p, -1, nullptr);
    }
    return t;
}

VASynthProcessor::KitDefinition VASynthProcessor::kitFromTree (const juce::ValueTree& t)
{
    KitDefinition def;
    if (! t.hasType ("KIT")) return def;
    def.name = t.getProperty ("name", juce::String()).toString();
    int idx = 0;
    for (auto p : t)
    {
        if (! p.hasType ("PAD") || idx >= kMaxKitPads) continue;
        auto& pd = def.pads[(std::size_t) idx++];
        pd.triggerNote = (int) p.getProperty ("trigger", -1);
        pd.source      = p.getProperty ("source", juce::String()).toString();
        pd.samplePath  = p.getProperty ("sample", juce::String()).toString();   // I2 (empty for synth pads / old kits)
        pd.numSound    = (int) p.getProperty ("num", 1);
        pd.level       = (float) p.getProperty ("level", 1.0);
        pd.chokeGroup  = (int) p.getProperty ("choke", 0);
        for (int s = 0; s < 4; ++s) pd.soundNote[(std::size_t) s] = (int) p.getProperty ("n" + juce::String (s), 60);
        if (p.getNumChildren() > 0) pd.voiceState = p.getChild (0).createCopy();   // edited pad voice, if any
    }
    return def;
}

// ---- factory kits -----------------------------------------------------------

juce::StringArray VASynthProcessor::factoryKitNames() { return { "808 Basics", "Stab Board" }; }

VASynthProcessor::KitDefinition VASynthProcessor::factoryKit (const juce::String& name)
{
    KitDefinition def; def.name = name;
    auto drum = [] (int trig, const char* src, int choke)
    { return KitPadDef { trig, src, { trig, 0, 0, 0 }, 1, 1.0f, choke }; };

    if (name == "808 Basics")
    {
        def.pads[0] = drum (36, "Kick 808",   0);
        def.pads[1] = drum (37, "Kick Punchy", 0);
        def.pads[2] = drum (38, "Snare",      0);
        def.pads[3] = drum (39, "Hat Closed", 1);            // hats mutually choke
        def.pads[4] = drum (40, "Hat Open",   1);
        def.pads[5] = drum (41, "Tom",        0);
    }
    else if (name == "Stab Board")
    {
        def.pads[0] = drum (36, "Kick 808",   0);
        def.pads[1] = drum (37, "Snare",      0);
        def.pads[2] = drum (38, "Hat Closed", 1);
        def.pads[3] = drum (39, "Hat Open",   1);
        // four tuned minor-triad stabs (root, +3, +7) on a plucky preset
        const int roots[4] = { 48, 50, 52, 53 };             // C D E F minor
        for (int i = 0; i < 4; ++i)
        {
            const int r = roots[i];
            def.pads[(std::size_t) (4 + i)] = KitPadDef { 40 + i, "Synth Pluck",
                                                          { r, r + 3, r + 7, 0 }, 3, 1.0f, 0 };
        }
    }
    return def;
}

// ---- kit presets (factory + user *.kit files) -------------------------------

juce::StringArray VASynthProcessor::getKitNames() const
{
    juce::StringArray names = factoryKitNames();
    for (auto& f : AppInfo::kitDir().findChildFiles (juce::File::findFiles, false, "*.kit"))
        names.add (f.getFileNameWithoutExtension());
    return names;
}

VASynthProcessor::KitDefinition VASynthProcessor::loadKit (const juce::String& name) const
{
    if (factoryKitNames().contains (name)) return factoryKit (name);
    auto file = AppInfo::kitDir().getChildFile (juce::File::createLegalFileName (name) + ".kit");
    if (auto xml = juce::XmlDocument::parse (file)) return kitFromTree (juce::ValueTree::fromXml (*xml));
    return {};
}

bool VASynthProcessor::saveKit (const juce::String& name, const KitDefinition& def)
{
    if (name.trim().isEmpty()) return false;
    KitDefinition d = def; d.name = name;
    auto file = AppInfo::kitDir().getChildFile (juce::File::createLegalFileName (name) + ".kit");
    if (auto xml = kitToTree (d).createXml()) return xml->writeTo (file);
    return false;
}

// Normalise a caller-supplied zone list into an ordered, contiguous, non-overlapping
// tiling of [0,127]: sort by lo, clamp, drop empties, snap each zone's start to the
// previous zone's end + 1, and stretch the ends of the first/last to cover the range.
// A malformed/empty list collapses to a single full-range LIVE zone.
static std::vector<VASynthProcessor::Zone> normaliseZones (std::vector<VASynthProcessor::Zone> z)
{
    using Zone = VASynthProcessor::Zone;
    for (auto& e : z) { e.loNote = juce::jlimit (0, 127, e.loNote); e.hiNote = juce::jlimit (0, 127, e.hiNote);
                        e.part = juce::jlimit (0, SynthEngine::maxParts - 1, e.part);
                        e.transpose = juce::jlimit (-60, 60, e.transpose); }
    z.erase (std::remove_if (z.begin(), z.end(), [] (const Zone& e) { return e.hiNote < e.loNote; }), z.end());
    if (z.empty()) return { Zone{} };
    std::sort (z.begin(), z.end(), [] (const Zone& a, const Zone& b) { return a.loNote < b.loNote; });
    z.front().loNote = 0;
    for (std::size_t i = 1; i < z.size(); ++i) z[i].loNote = juce::jlimit (0, 127, z[i - 1].hiNote + 1);
    // Drop any zone the previous one now swallowed, then re-close gaps.
    z.erase (std::remove_if (z.begin(), z.end(), [] (const Zone& e) { return e.hiNote < e.loNote; }), z.end());
    for (std::size_t i = 0; i + 1 < z.size(); ++i) z[i].hiNote = juce::jlimit (z[i].loNote, 127, z[i].hiNote);
    z.back().hiNote = 127;
    return z;
}

void VASynthProcessor::setSurfaceZones (const juce::String& surface, std::vector<Zone> zones)
{
    auto norm = normaliseZones (std::move (zones));
    const juce::ScopedLock sl (zoneLock);
    for (auto& e : surfaceZones) if (e.first == surface) { e.second = std::move (norm); return; }
    surfaceZones.emplace_back (surface, std::move (norm));
}

std::vector<VASynthProcessor::Zone> VASynthProcessor::getSurfaceZones (const juce::String& surface) const
{
    const juce::ScopedLock sl (zoneLock);
    for (auto& e : surfaceZones) if (e.first == surface) return e.second;
    return {};                                       // implicit default (single LIVE zone)
}

bool VASynthProcessor::surfaceHasSplit (const juce::String& surface) const
{
    const juce::ScopedLock sl (zoneLock);
    for (auto& e : surfaceZones) if (e.first == surface) return e.second.size() > 1;
    return false;
}

void VASynthProcessor::resetSurfaceZones (const juce::String& surface)
{
    const juce::ScopedLock sl (zoneLock);
    for (auto& e : surfaceZones) if (e.first == surface) { e.second = { Zone{} }; return; }
}

void VASynthProcessor::resetAllRouting()
{
    const juce::ScopedLock sl (zoneLock);
    surfaceZones.clear();
    noteLedger.clear();
}

void VASynthProcessor::addSurfaceSplit (const juce::String& surface, int seamNote)
{
    seamNote = juce::jlimit (1, 127, seamNote);          // note 0 can't start a new zone
    auto z = getSurfaceZones (surface);
    if (z.empty()) z = { Zone{} };
    for (std::size_t i = 0; i < z.size(); ++i)
        if (seamNote > z[i].loNote && seamNote <= z[i].hiNote)
        {
            Zone right = z[i];
            right.loNote = seamNote; right.part = 0; right.transpose = 0;   // new zone starts on LIVE
            z[i].hiNote = seamNote - 1;
            z.insert (z.begin() + (long) i + 1, right);
            break;
        }
    setSurfaceZones (surface, std::move (z));
}

void VASynthProcessor::removeSurfaceSplit (const juce::String& surface, int zoneIndex)
{
    auto z = getSurfaceZones (surface);
    if ((int) z.size() <= 1 || zoneIndex < 0 || zoneIndex >= (int) z.size()) return;
    // Drop the boundary: the removed zone's range folds into the previous one (or the
    // next, if it was the first zone). setSurfaceZones re-closes the tiling.
    if (zoneIndex == 0) z[1].loNote = z[0].loNote;
    else                z[(std::size_t) zoneIndex - 1].hiNote = z[(std::size_t) zoneIndex].hiNote;
    z.erase (z.begin() + zoneIndex);
    setSurfaceZones (surface, std::move (z));
}

VASynthProcessor::Zone VASynthProcessor::resolveZone (const juce::String& surface, int note) const
{
    const juce::ScopedLock sl (zoneLock);
    for (auto& e : surfaceZones)
        if (e.first == surface)
        {
            for (auto& z : e.second) if (note >= z.loNote && note <= z.hiNote) return z;
            break;
        }
    return Zone{};                                   // default: full-range LIVE, no transpose
}

void VASynthProcessor::setSurfaceRouting (const juce::String& surface, int part)
{
    if (part < 0 || part >= SynthEngine::maxParts) return;
    setSurfaceZones (surface, { Zone{ 0, 127, part, 0 } });   // whole surface -> one part
}

int VASynthProcessor::getSurfaceRouting (const juce::String& surface) const
{
    return resolveZone (surface, 60).part;           // single-zone part, or the zone at middle C
}

void VASynthProcessor::routeSurfaceMessage (const juce::String& surface, const juce::MidiMessage& m)
{
    bumpSurfaceActivity (surface);
    pushMonitor (surface, m, getSurfaceRouting (surface));   // F12 diagnostic: exact surface/ch/note

    if (m.isNoteOn())
    {
        const int note = m.getNoteNumber();
        lastAnyNoteVal.store (note, std::memory_order_relaxed);          // learn-by-play (any surface)
        lastAnyNoteSeq.fetch_add (1, std::memory_order_relaxed);
        {
            const juce::ScopedLock sl (routingLock);     // last note per surface (split-by-play)
            bool set = false;
            for (auto& e : surfaceNotes) if (e.first == surface) { e.second = note; set = true; break; }
            if (! set) surfaceNotes.emplace_back (surface, note);
        }
        const Zone z = resolveZone (surface, note);
        const int sounding = juce::jlimit (0, 127, note + z.transpose);
        {
            const juce::ScopedLock sl (zoneLock);    // record what this note-on triggered
            noteLedger.push_back ({ surface, note, z.part, sounding });
        }
        routeNoteOn (sounding, m.getFloatVelocity(), z.part);
    }
    else if (m.isNoteOff())
    {
        const int note = m.getNoteNumber();
        int part = 0, sounding = note;
        bool found = false;
        {
            const juce::ScopedLock sl (zoneLock);    // release exactly what the note-on triggered
            for (auto it = noteLedger.rbegin(); it != noteLedger.rend(); ++it)
                if (it->surface == surface && it->note == note)
                { part = it->part; sounding = it->sounding; noteLedger.erase (std::next (it).base()); found = true; break; }
        }
        if (! found) { const Zone z = resolveZone (surface, note); part = z.part; sounding = juce::jlimit (0, 127, note + z.transpose); }
        routeNoteOff (sounding, part);
    }
    else
        routeMidi (m, getSurfaceRouting (surface));  // CC / pitch-bend: tagged with THIS surface's part (G6)
}

// ---- MULTI layouts: a named snapshot of parts + surface zones ---------------

// Capture the focused part's current live sound into its store, so a MULTI/session save
// reflects edits made to a locked part that hasn't been focused away from yet.
void VASynthProcessor::syncFocusedPartState()
{
    const int f = editFocusPart.load (std::memory_order_relaxed);
    if (f > 0 && f < SynthEngine::maxParts) partEditState[(std::size_t) f] = apvts.copyState();
}

juce::ValueTree VASynthProcessor::captureMultiState()
{
    syncFocusedPartState();
    juce::ValueTree multi ("MULTI");
    for (int p = 1; p < SynthEngine::maxParts; ++p)
    {
        if (isPartKit (p))                               // a kit part serialises its whole definition
        {
            auto kt = kitToTree (partKits[(std::size_t) p]);
            kt.setProperty ("index", p, nullptr);
            multi.addChild (kt, -1, nullptr);
        }
        else if (partPresetName[(std::size_t) p].isNotEmpty())
        {
            juce::ValueTree e ("PART");
            e.setProperty ("index", p, nullptr);
            e.setProperty ("preset", partPresetName[(std::size_t) p], nullptr);
            // 1.3: an EDITED part carries its full custom sound so recall restores the tweaks,
            // not the clean preset.
            if (partEdited[(std::size_t) p] && partEditState[(std::size_t) p].isValid())
            {
                e.setProperty ("edited", true, nullptr);
                e.addChild (partEditState[(std::size_t) p].createCopy(), -1, nullptr);
            }
            multi.addChild (e, -1, nullptr);
        }
    }
    const juce::ScopedLock sl (zoneLock);
    for (auto& sz : surfaceZones)
    {
        juce::ValueTree s ("SURFACE");
        s.setProperty ("name", sz.first, nullptr);
        for (auto& z : sz.second)
        {
            juce::ValueTree zt ("ZONE");
            zt.setProperty ("lo", z.loNote, nullptr);   zt.setProperty ("hi", z.hiNote, nullptr);
            zt.setProperty ("part", z.part, nullptr);   zt.setProperty ("transpose", z.transpose, nullptr);
            s.addChild (zt, -1, nullptr);
        }
        multi.addChild (s, -1, nullptr);
    }

    // Part mixer (level/pan per part) travels with the MULTI.
    namespace ID = ParamID;
    const char* lvlIDs[] { ID::part0Level, ID::part1Level, ID::part2Level, ID::part3Level };
    const char* panIDs[] { ID::part0Pan,   ID::part1Pan,   ID::part2Pan,   ID::part3Pan   };
    juce::ValueTree mix ("MIX");
    for (int p = 0; p < SynthEngine::maxParts; ++p)
    {
        mix.setProperty ("l" + juce::String (p), rp (apvts, lvlIDs[p]), nullptr);
        mix.setProperty ("p" + juce::String (p), rp (apvts, panIDs[p]), nullptr);
    }
    multi.addChild (mix, -1, nullptr);
    return multi;
}

void VASynthProcessor::applyMultiState (const juce::ValueTree& multi)
{
    if (! multi.hasType ("MULTI")) return;

    // Start from a clean layout, then apply. Parts not named in the MULTI go back to
    // unassigned (locked + kit cleared); every surface's zones are replaced.
    resetAllRouting();
    std::array<bool, SynthEngine::maxParts> partOk {};
    for (int p = 1; p < SynthEngine::maxParts; ++p)
    {
        engine.clearPartKit (p);
        partKits[(std::size_t) p] = KitDefinition{};
        partPresetName[(std::size_t) p].clear();
    }

    for (auto e : multi)
    {
        if (e.hasType ("PART"))
        {
            const int p = (int) e.getProperty ("index", -1);
            if (p >= 1 && p < SynthEngine::maxParts)
            {
                partOk[(std::size_t) p] = setPartPreset (p, e.getProperty ("preset", juce::String()).toString());
                // 1.3: restore an edited part's custom sound over the clean preset.
                if ((bool) e.getProperty ("edited", false))
                    for (auto child : e)
                        if (child.getType().toString() == apvts.state.getType().toString())
                        {
                            partEditState[(std::size_t) p] = child.createCopy();
                            partEdited[(std::size_t) p] = true;
                            bakeStateToSlot (p, partEditState[(std::size_t) p]);
                            if (editFocusPart.load (std::memory_order_relaxed) == p)
                                applyPartSoundFromTree (partEditState[(std::size_t) p]);
                            break;
                        }
            }
        }
        else if (e.hasType ("KIT"))
        {
            const int p = (int) e.getProperty ("index", -1);
            if (p >= 1 && p < SynthEngine::maxParts) { setPartKit (p, kitFromTree (e)); partOk[(std::size_t) p] = true; }
        }
        else if (e.hasType ("SURFACE"))
        {
            std::vector<Zone> zones;
            for (auto zt : e)
                if (zt.hasType ("ZONE"))
                    zones.push_back ({ (int) zt.getProperty ("lo", 0),   (int) zt.getProperty ("hi", 127),
                                       (int) zt.getProperty ("part", 0), (int) zt.getProperty ("transpose", 0) });

            // A zone pointing at a part whose named preset was missing falls back to LIVE.
            for (auto& z : zones)
                if (z.part >= 1 && z.part < SynthEngine::maxParts && ! partOk[(std::size_t) z.part])
                {
                    juce::Logger::writeToLog ("MULTI: surface '" + e.getProperty ("name", juce::String()).toString()
                                              + "' zone on part " + juce::String (z.part) + " has no preset -> LIVE");
                    z.part = 0;
                }
            setSurfaceZones (e.getProperty ("name", juce::String()).toString(), std::move (zones));
        }
        else if (e.hasType ("MIX"))
        {
            namespace ID = ParamID;
            const char* lvlIDs[] { ID::part0Level, ID::part1Level, ID::part2Level, ID::part3Level };
            const char* panIDs[] { ID::part0Pan,   ID::part1Pan,   ID::part2Pan,   ID::part3Pan   };
            auto set = [this] (const char* id, float v)
            { if (auto* prm = apvts.getParameter (id)) prm->setValueNotifyingHost (prm->convertTo0to1 (v)); };
            for (int p = 0; p < SynthEngine::maxParts; ++p)
            {
                set (lvlIDs[p], (float) e.getProperty ("l" + juce::String (p), 1.0));
                set (panIDs[p], (float) e.getProperty ("p" + juce::String (p), 0.0));
            }
        }
    }
}

bool VASynthProcessor::saveMulti (const juce::String& name)
{
    if (name.trim().isEmpty()) return false;
    auto file = AppInfo::multiDir().getChildFile (juce::File::createLegalFileName (name) + ".multi");
    if (auto xml = captureMultiState().createXml())
        return xml->writeTo (file);
    return false;
}

bool VASynthProcessor::loadMulti (const juce::String& name)
{
    auto file = AppInfo::multiDir().getChildFile (juce::File::createLegalFileName (name) + ".multi");
    if (auto xml = juce::XmlDocument::parse (file))
    {
        applyMultiState (juce::ValueTree::fromXml (*xml));
        return true;
    }
    return false;
}

juce::StringArray VASynthProcessor::getMultiNames() const
{
    juce::StringArray names;
    for (auto& f : AppInfo::multiDir().findChildFiles (juce::File::findFiles, false, "*.multi"))
        names.add (f.getFileNameWithoutExtension());
    names.sortNatural();
    return names;
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

int VASynthProcessor::lastNoteForSurface (const juce::String& surface) const
{
    const juce::ScopedLock sl (routingLock);
    for (auto& e : surfaceNotes) if (e.first == surface) return e.second;
    return -1;
}

void VASynthProcessor::dispatchNoteOn (int note, float vel, int part, bool chordOn, bool generator)
{
    if (part >= 0 && part < SynthEngine::maxParts)
        partHits[(std::size_t) part].fetch_add (1, std::memory_order_relaxed);   // PARTS strip flicker

    if (part == playFocusPart.load (std::memory_order_relaxed) && chordOn)   // chord = focused/live part
    {
        const int mod = modifierLearn.handleNoteOn (note);   // learned pad consumed
        if (mod >= 0)
        {
            midiModMask |= (1u << mod);
            applyChordModifiers (qwertyModMask.load (std::memory_order_acquire) | midiModMask);
            return;
        }
        int trig[ChordEngine::kMaxTones], rel[ChordEngine::kMaxTones]; int nt = 0, nr = 0;
        chordEngine.noteOn (note, vel, trig, nt, rel, nr);
        for (int i = 0; i < nr; ++i) engine.noteOff (rel[i], part);
        for (int i = 0; i < nt; ++i) engine.noteOn (trig[i], vel, part, 0, generator);
    }
    else if (engine.partIsKit (part))
    {
        partLastTrig[(std::size_t) part].store (note, std::memory_order_relaxed);   // pad flicker
        engine.kitNoteOn (part, note, vel, generator);   // trigger -> pad (sounding notes + choke)
    }
    else
        engine.noteOn (note, vel, part, 0, generator);   // locked parts: chord is live-only
}

void VASynthProcessor::dispatchNoteOff (int note, int part, bool chordOn)
{
    if (part == playFocusPart.load (std::memory_order_relaxed) && chordOn)   // chord = focused/live part
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
        for (int i = 0; i < nr; ++i) engine.noteOff (rel[i], part);
    }
    else if (engine.partIsKit (part))
        engine.kitNoteOff (part, note);
    else
        engine.noteOff (note, part);
}

// CC / pitch-bend / all-notes-off — shared by the host `midi` buffer and the routed
// FIFO. These act globally / on the live part (v1: control is not per-part).
void VASynthProcessor::handleControlMessage (const juce::MidiMessage& msg, int part)
{
    if (msg.isAllNotesOff() || msg.isAllSoundOff())
    {
        engine.allNotesOff(); chordEngine.reset(); midiModMask = 0; lastFedModMask = 0;
        return;
    }
    if (msg.isPitchWheel())
    {
        pitchBendEvents.fetch_add (1, std::memory_order_relaxed);       // G6 intake trace
        const float norm = (msg.getPitchWheelValue() - 8192) / 8192.0f;
        const float semis = norm * pitchBendRangeSemis.load (std::memory_order_acquire);
        if (part < 0) engine.setPitchBend (semis); else engine.setPitchBend (part, semis);   // per routed part
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
            if (cc == 1) { const float amt = val / 127.0f;                                        // mod wheel -> vibrato (+ G6 trace)
                           if (part < 0) engine.setModWheel (amt); else engine.setModWheel (part, amt);
                           modWheelEvents.fetch_add (1, std::memory_order_relaxed); }
            midiLearn.handleCC (msg.getChannel(), cc, val);
        }
    }
}

void VASynthProcessor::applyBlockMods (int part, VoiceParams& vp, FXParams& fx, PartLfos& lfo)
{
    const int p = juce::jlimit (0, SynthEngine::maxParts - 1, part);
    const auto& mtx = partMatrix[(std::size_t) p];
    if (! mtx.active())                              // fully inert -> zero-cost (golden-safe)
    {
        for (auto& a : blockOffsetPub) a.store (0.0f, std::memory_order_relaxed);   // clear stale UI animation
        voiceOffCutoffOct.store (0.0f, std::memory_order_relaxed); voiceOffPw.store (0.0f, std::memory_order_relaxed);
        voiceOffReso.store (0.0f, std::memory_order_relaxed);
        for (auto& a : voiceOffOscLvl) a.store (0.0f, std::memory_order_relaxed);
        return;
    }

    namespace ID = ParamID;
    // Block-level sources only (per-voice sources — velocity/env/note — have no defined value
    // at block scope; LFO block sources land in a follow-up). Macros + wheel + bend cover the
    // headline uses (Macro -> delay feedback, wheel -> reverb mix, ...).
    ModSources s;
    const char* mids[8] { ID::macro1, ID::macro2, ID::macro3, ID::macro4,
                          ID::macro5, ID::macro6, ID::macro7, ID::macro8 };
    for (int i = 0; i < 8; ++i) s.macro[(std::size_t) i] = rp (apvts, mids[i]);
    s.modWheel = engine.modWheelAmount (p);
    const float bendRange = pitchBendRangeSemis.load (std::memory_order_acquire);
    s.pitchBend = bendRange > 0.0f ? engine.pitchBendSemitones (p) / bendRange : 0.0f;
    for (int k = 0; k < 3; ++k) s.lfo[(std::size_t) k] = engine.focusLfoRawOut (k);   // block-tier LFO sources (1-block latency)

    float off[ModMatrix::kNumBlockDests];
    mtx.blockOffsets (s, off, ModMatrix::kNumBlockDests);
    for (int i = 0; i < ModMatrix::kNumBlockDests; ++i)
        blockOffsetPub[(std::size_t) i].store (off[(std::size_t) i], std::memory_order_relaxed);   // publish for UI/tests

    // Voice-tier offsets (from the same block-level sources) for UI animation of cutoff/reso/pw/levels.
    const auto vo = mtx.evaluate (s);
    voiceOffCutoffOct.store (vo.cutoffOct, std::memory_order_relaxed);
    voiceOffPw.store   (vo.pw,   std::memory_order_relaxed);
    voiceOffReso.store (vo.reso, std::memory_order_relaxed);
    voiceOffOscLvl[0].store (vo.osc1Level, std::memory_order_relaxed);
    voiceOffOscLvl[1].store (vo.osc2Level, std::memory_order_relaxed);
    voiceOffOscLvl[2].store (vo.osc3Level, std::memory_order_relaxed);

    // Add the normalized offset to the param's normalized base, then convert back through the
    // param's own range (respects log/skew), and write the natural value to the engine struct.
    auto mod = [&] (int dest, const char* pid, float& field)
    {
        const int idx = dest - ModMatrix::kFirstBlockDest;
        if (idx < 0 || idx >= ModMatrix::kNumBlockDests || off[(std::size_t) idx] == 0.0f) return;
        if (auto* prm = apvts.getParameter (pid))
        {
            const auto& r = prm->getNormalisableRange();
            field = r.convertFrom0to1 (juce::jlimit (0.0f, 1.0f, prm->getValue() + off[(std::size_t) idx]));
        }
    };

    // FX
    mod (ModMatrix::ChorusRate,    ID::chorusRate,    fx.chorusRate);
    mod (ModMatrix::ChorusDepth,   ID::chorusDepth,   fx.chorusDepth);
    mod (ModMatrix::ChorusMix,     ID::chorusMix,     fx.chorusMix);
    mod (ModMatrix::DelayTime,     ID::delayTime,     fx.delayTimeMs);
    mod (ModMatrix::DelayFeedback, ID::delayFeedback, fx.delayFeedback);
    mod (ModMatrix::DelayMix,      ID::delayMix,      fx.delayMix);
    mod (ModMatrix::DelaySpread,   ID::delaySpread,   fx.delaySpread);
    mod (ModMatrix::ReverbSize,    ID::reverbSize,    fx.reverbSize);
    mod (ModMatrix::ReverbDamp,    ID::reverbDamp,    fx.reverbDamp);
    mod (ModMatrix::ReverbWidth,   ID::reverbWidth,   fx.reverbWidth);
    mod (ModMatrix::ReverbMix,     ID::reverbMix,     fx.reverbMix);
    mod (ModMatrix::StereoWidth,   ID::stereoWidth,   fx.width);
    mod (ModMatrix::EqB1Gain,      ID::peqB1Gain,     fx.eqBand1.gainDb);
    mod (ModMatrix::EqB2Gain,      ID::peqB2Gain,     fx.eqBand2.gainDb);
    mod (ModMatrix::EqB3Gain,      ID::peqB3Gain,     fx.eqBand3.gainDb);
    mod (ModMatrix::EqB4Gain,      ID::peqB4Gain,     fx.eqBand4.gainDb);
    mod (ModMatrix::EqB5Gain,      ID::peqB5Gain,     fx.eqBand5.gainDb);
    // LFO rate/depth
    mod (ModMatrix::Lfo1Rate,  ID::lfoRate,   lfo.lfo[0].rate);
    mod (ModMatrix::Lfo1Depth, ID::lfoDepth,  lfo.lfo[0].depth);
    mod (ModMatrix::Lfo2Rate,  ID::lfo2Rate,  lfo.lfo[1].rate);
    mod (ModMatrix::Lfo2Depth, ID::lfo2Depth, lfo.lfo[1].depth);
    mod (ModMatrix::Lfo3Rate,  ID::lfo3Rate,  lfo.lfo[2].rate);
    mod (ModMatrix::Lfo3Depth, ID::lfo3Depth, lfo.lfo[2].depth);
    // Envelopes
    mod (ModMatrix::AmpAttack,  ID::ampAttack,  vp.ampA);
    mod (ModMatrix::AmpDecay,   ID::ampDecay,   vp.ampD);
    mod (ModMatrix::AmpSustain, ID::ampSustain, vp.ampS);
    mod (ModMatrix::AmpRelease, ID::ampRelease, vp.ampR);
    mod (ModMatrix::FltAttack,  ID::fltAttack,  vp.fltA);
    mod (ModMatrix::FltDecay,   ID::fltDecay,   vp.fltD);
    mod (ModMatrix::FltSustain, ID::fltSustain, vp.fltS);
    mod (ModMatrix::FltRelease, ID::fltRelease, vp.fltR);
    // Filter + env-amount
    mod (ModMatrix::FilterEnvAmt,   ID::filterEnvAmt,  vp.filterEnvAmt);
    mod (ModMatrix::FilterKeytrack, ID::filterKeytrack,vp.keytrack);
    mod (ModMatrix::VelToCutoff,    ID::velToCutoff,   vp.velToCutoff);
    mod (ModMatrix::VelToAmp,       ID::velToAmp,      vp.velToAmp);
    mod (ModMatrix::FltEnvToPitch,  ID::fltEnvToPitch, vp.fltEnvToPitch);
    // Osc tune
    mod (ModMatrix::Osc1Octave, ID::osc1Octave, vp.osc1Octave);
    mod (ModMatrix::Osc1Detune, ID::osc1Detune, vp.osc1Detune);
    mod (ModMatrix::Osc2Octave, ID::osc2Octave, vp.osc2Octave);
    mod (ModMatrix::Osc2Detune, ID::osc2Detune, vp.osc2Detune);
    mod (ModMatrix::Osc3Octave, ID::osc3Octave, vp.osc3Octave);
    mod (ModMatrix::Osc3Detune, ID::osc3Detune, vp.osc3Detune);
    // Glide
    mod (ModMatrix::GlideTime, ID::glideTime, vp.glideTime);
}

void VASynthProcessor::drainRoutedMidi (bool chordOn, int focus)
{
    int start1, size1, start2, size2;
    routedFifo.prepareToRead (routedFifo.getNumReady(), start1, size1, start2, size2);
    auto handle = [&] (int idx)
    {
        const auto& e = routedBuf[(std::size_t) idx];
        const juce::MidiMessage m (e.status, e.d1, e.d2);
        // The LIVE zone resolves to part 0; edit-focus remaps it to the focused part so the
        // main keyboard plays (and edits) whatever part is in focus (1.3). Surfaces routed
        // to an explicit part are unaffected.
        const int part = (e.part == 0) ? focus : e.part;
        // When the arp is on, the LIVE/focused part's played notes feed the arp instead of
        // sounding directly; it re-emits them on its clock. Locked parts + control unaffected.
        const bool arpCapture = arp.enabled() && part == focus;
        if (m.isNoteOn())       { looper.recordNote (part, 0, e.d1, e.d2 / 127.0f, true);  if (arpCapture) arp.noteOn (e.d1, e.d2 / 127.0f); else dispatchNoteOn  (e.d1, e.d2 / 127.0f, part, chordOn); }
        else if (m.isNoteOff()) { looper.recordNote (part, 0, e.d1, 0.0f, false);           if (arpCapture) arp.noteOff (e.d1);               else dispatchNoteOff (e.d1, part, chordOn); }
        else                    handleControlMessage (m, part);   // bend/CC1 hit the routed part (G6)
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

    // K1: the per-part EQ (fixed last stage). The LIVE part reads it here — this was the
    // missing wiring: snapshotFXParams never carried the EQ, so the live part's EQ did
    // nothing until now. Locked parts get it via fxParamsFrom (scratch APVTS).
    p.eqBand1 = { rp (apvts, ID::peqB1Freq), rp (apvts, ID::peqB1Gain), rp (apvts, ID::peqB1Q), rp (apvts, ID::peqB1On) > 0.5f };
    p.eqBand2 = { rp (apvts, ID::peqB2Freq), rp (apvts, ID::peqB2Gain), rp (apvts, ID::peqB2Q), rp (apvts, ID::peqB2On) > 0.5f };
    p.eqBand3 = { rp (apvts, ID::peqB3Freq), rp (apvts, ID::peqB3Gain), rp (apvts, ID::peqB3Q), rp (apvts, ID::peqB3On) > 0.5f };
    p.eqBand4 = { rp (apvts, ID::peqB4Freq), rp (apvts, ID::peqB4Gain), rp (apvts, ID::peqB4Q), rp (apvts, ID::peqB4On) > 0.5f };
    p.eqBand5 = { rp (apvts, ID::peqB5Freq), rp (apvts, ID::peqB5Gain), rp (apvts, ID::peqB5Q), rp (apvts, ID::peqB5On) > 0.5f };

    p.enabled[FXChain::Chorus_] = rp (apvts, ID::fxChorusOn) > 0.5f;
    p.enabled[FXChain::Delay_]  = rp (apvts, ID::fxDelayOn)  > 0.5f;
    p.enabled[FXChain::Reverb_] = rp (apvts, ID::fxReverbOn) > 0.5f;
    p.enabled[FXChain::Width_]  = rp (apvts, ID::fxWidthOn)  > 0.5f;
    p.enabled[FXChain::EQ_]     = rp (apvts, ID::peqOn)      > 0.5f;

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
    // Accept the current 5-slot order, or a legacy 4-slot order (pre-EQ) which we pad by
    // appending the missing ids (e.g. EQ=4) so an old session still loads.
    if (tokens.size() < 4 || tokens.size() > kFxCount) return;
    int order[kFxCount] { 0, 1, 2, 3, 4 };
    bool seen[kFxCount] = {};
    for (int i = 0; i < tokens.size(); ++i) { order[i] = tokens[i].getIntValue(); if (order[i] >= 0 && order[i] < kFxCount) seen[order[i]] = true; }
    for (int i = tokens.size(); i < kFxCount; ++i)
        for (int v = 0; v < kFxCount; ++v) if (! seen[v]) { order[i] = v; seen[v] = true; break; }
    setFxOrder (order);        // validates the permutation; ignores if malformed
}

// --- macros ------------------------------------------------------------------

juce::StringArray VASynthProcessor::macroRoutableIDs()
{
    namespace ID = ParamID;
    return { ID::filterCutoff, ID::filterReso, ID::filterEnvAmt, ID::filterKeytrack,
             ID::osc1Detune, ID::osc2Detune, ID::osc1PW, ID::osc2PW,
             ID::chorusMix, ID::chorusRate, ID::delayMix, ID::delayFeedback,
             ID::reverbMix, ID::reverbSize, ID::stereoWidth,
             ID::lfoDepth, ID::lfo2Depth, ID::glideTime, ID::ampRelease,
             ID::fltEnvToPitch, ID::velToCutoff };
}

void VASynthProcessor::writeMacroMapProperty()
{
    juce::StringArray ids;
    for (auto& id : macroTargetId) ids.add (id.isEmpty() ? "-" : id);
    apvts.state.setProperty ("macro_map", ids.joinIntoString (","), nullptr);
}

std::array<juce::String, 8> VASynthProcessor::defaultMacroTargets()
{
    return { ParamID::filterCutoff, ParamID::filterReso, ParamID::filterEnvAmt, ParamID::ampRelease,
             ParamID::lfoRate, ParamID::lfoDepth, ParamID::reverbMix, juce::String (kFocusLevelTarget) };
}

void VASynthProcessor::applyMacroMapProperty()
{
    for (auto& t : macroTargetId) t.clear();
    const auto str = apvts.state.getProperty ("macro_map").toString();
    if (str.isEmpty()) { macroTargetId = defaultMacroTargets(); return; }   // pre-#55 state -> factory map
    auto tokens = juce::StringArray::fromTokens (str, ",", "");
    for (int i = 0; i < juce::jmin (8, tokens.size()); ++i)
        if (tokens[i] == kFocusLevelTarget || (tokens[i] != "-" && apvts.getParameter (tokens[i]) != nullptr))
            macroTargetId[(std::size_t) i] = tokens[i];
}

void VASynthProcessor::writeMacroNamesProperty()
{
    juce::StringArray ns;
    for (auto& n : macroName) ns.add (n.isEmpty() ? "-" : n);
    apvts.state.setProperty ("macro_names", ns.joinIntoString ("\t"), nullptr);   // tab-joined (names may hold commas)
}

void VASynthProcessor::applyMacroNamesProperty()
{
    for (auto& n : macroName) n.clear();
    const auto str = apvts.state.getProperty ("macro_names").toString();
    if (str.isEmpty()) return;
    auto tokens = juce::StringArray::fromTokens (str, "\t", "");
    for (int i = 0; i < juce::jmin (8, tokens.size()); ++i)
        if (tokens[i] != "-") macroName[(std::size_t) i] = tokens[i];
}

juce::StringArray VASynthProcessor::macroDestinationNames (int i) const
{
    juce::StringArray out;
    if (i < 0 || i >= 8) return out;
    if (macroTargetId[(std::size_t) i].isNotEmpty()) out.add (getMacroTargetName (i));
    const auto& m = partMatrix[(std::size_t) juce::jlimit (0, SynthEngine::maxParts - 1, editFocus())];
    for (auto& s : m.slots)
        if (s.source == ModMatrix::Macro1 + i && s.dest != ModMatrix::DstNone)
            out.add (moddest::nameFor (s.dest));
    return out;
}

// Mod matrix (#56): "src:dest:depth,...(8 slots)" per part, parts joined by ';'.
void VASynthProcessor::writeModMatrixProperty()
{
    juce::StringArray parts;
    for (auto& m : partMatrix)
    {
        juce::StringArray slots;
        for (auto& s : m.slots)
            slots.add (juce::String (s.source) + ":" + juce::String (s.dest) + ":" + juce::String (s.depth, 4));
        parts.add (slots.joinIntoString (","));
    }
    apvts.state.setProperty ("mod_matrix", parts.joinIntoString (";"), nullptr);
}

void VASynthProcessor::applyModMatrixProperty()
{
    for (auto& m : partMatrix) m = ModMatrix{};
    const auto str = apvts.state.getProperty ("mod_matrix").toString();
    if (str.isEmpty()) return;                                  // absent -> all inert (default)
    auto parts = juce::StringArray::fromTokens (str, ";", "");
    for (int p = 0; p < juce::jmin ((int) partMatrix.size(), parts.size()); ++p)
    {
        auto slots = juce::StringArray::fromTokens (parts[p], ",", "");
        for (int i = 0; i < juce::jmin (kModSlots, slots.size()); ++i)
        {
            auto f = juce::StringArray::fromTokens (slots[i], ":", "");
            if (f.size() >= 3)
                partMatrix[(std::size_t) p].slots[(std::size_t) i]
                    = { juce::jlimit (0, ModMatrix::kNumSources - 1, f[0].getIntValue()),
                        juce::jlimit (0, ModMatrix::kNumDests   - 1, f[1].getIntValue()),
                        juce::jlimit (-1.0f, 1.0f, f[2].getFloatValue()) };
        }
    }
}

void VASynthProcessor::writeArpStepsProperty()
{
    juce::StringArray on;  for (float s : arpSteps)          on.add  (s > 0.5f ? "1" : "0");
    apvts.state.setProperty ("arp_steps", on.joinIntoString (","), nullptr);
    juce::StringArray vel; for (unsigned char v : arpStepVel) vel.add (juce::String ((int) v));   // per-step vel %
    apvts.state.setProperty ("arp_vel", vel.joinIntoString (","), nullptr);
}

void VASynthProcessor::applyArpStepsProperty()
{
    auto tokens = juce::StringArray::fromTokens (apvts.state.getProperty ("arp_steps").toString(), ",", "");
    // arp_vel is the per-step velocity list (task #54). If absent (older state, or a state
    // saved during the brief single-VEL-knob build), every on-step loads at the default 100 %
    // — the retired knob's global scale is intentionally NOT migrated (documented reset).
    auto vels = juce::StringArray::fromTokens (apvts.state.getProperty ("arp_vel").toString(), ",", "");
    const bool hasVel = vels.size() >= kArpSteps;
    for (int i = 0; i < kArpSteps; ++i)
    {
        // Old states stored a float level (0..1, default 0.8 = on); anything > 0.5 is "on".
        arpSteps[(std::size_t) i]   = (i < tokens.size()) ? (tokens[i].getFloatValue() > 0.5f ? 1.0f : 0.0f)
                                                          : 1.0f;   // default: all steps on
        arpStepVel[(std::size_t) i] = (unsigned char) (hasVel ? juce::jlimit (0, 200, vels[i].getIntValue()) : 0);
    }
}

void VASynthProcessor::writeSeqProperty()
{
    juce::String cells;
    for (auto& row : seqCells) for (unsigned char c : row) cells << (int) (c ? 1 : 0);   // 128 digits 0/1
    apvts.state.setProperty ("seq_cells", cells, nullptr);
    juce::StringArray vels; for (auto& row : seqVel) for (unsigned char v : row) vels.add (juce::String ((int) v));   // per-step vel %
    apvts.state.setProperty ("seq_vel", vels.joinIntoString (","), nullptr);
    juce::StringArray notes; for (int n : seqNotes) notes.add (juce::String (n));
    apvts.state.setProperty ("seq_notes", notes.joinIntoString (","), nullptr);
    juce::String mutes; for (bool m : seqMutes) mutes << (m ? "1" : "0");
    apvts.state.setProperty ("seq_mutes", mutes, nullptr);
}

void VASynthProcessor::applySeqProperty()
{
    const auto cells = apvts.state.getProperty ("seq_cells").toString();
    // seq_vel is a per-step velocity list (task #54). If absent (older state), migrate from
    // the legacy 3-state cells: accent (2) -> velocity 127, else default (0 => 100%).
    auto vels = juce::StringArray::fromTokens (apvts.state.getProperty ("seq_vel").toString(), ",", "");
    const bool hasVel = vels.size() >= kSeqRows * kSeqSteps;
    for (int r = 0; r < kSeqRows; ++r)
        for (int s = 0; s < kSeqSteps; ++s)
        {
            const int idx = r * kSeqSteps + s;
            const int v = (idx < cells.length()) ? (cells[idx] - '0') : 0;   // legacy 0/1/2
            seqCells[(std::size_t) r][(std::size_t) s] = (unsigned char) (v == 0 ? 0 : 1);
            seqVel[(std::size_t) r][(std::size_t) s] = (unsigned char) (hasVel ? juce::jlimit (0, 200, vels[idx].getIntValue())
                                                                               : (v == 2 ? 127 : 0));   // migrate accent
        }
    auto notes = juce::StringArray::fromTokens (apvts.state.getProperty ("seq_notes").toString(), ",", "");
    for (int r = 0; r < kSeqRows; ++r)
        seqNotes[(std::size_t) r] = (r < notes.size()) ? juce::jlimit (0, 127, notes[r].getIntValue()) : (36 + r);
    const auto mutes = apvts.state.getProperty ("seq_mutes").toString();
    for (int r = 0; r < kSeqRows; ++r) seqMutes[(std::size_t) r] = (r < mutes.length()) && (mutes[r] == '1');
}

bool VASynthProcessor::exportLoopsToMidiFile (const juce::File& file) const
{
    bool any = false;
    for (int p = 0; p < Looper::kParts; ++p) any = any || looper.hasContent (p);
    if (! any) return false;

    const double bpm = juce::jmax (20.0f, apvts.getRawParameterValue (ParamID::tempo)->load());
    const double sr  = getSampleRate() > 0.0 ? getSampleRate() : 48000.0;
    const int    ppq = 960;
    const double samplesPerBeat = sr * 60.0 / bpm;

    juce::MidiFile mf;
    mf.setTicksPerQuarterNote (ppq);
    {
        juce::MidiMessageSequence tempoTrack;                       // track 0: tempo map
        tempoTrack.addEvent (juce::MidiMessage::tempoMetaEvent ((int) (60000000.0 / bpm)), 0.0);
        mf.addTrack (tempoTrack);
    }
    for (int p = 0; p < Looper::kParts; ++p)
    {
        if (! looper.hasContent (p)) continue;
        juce::MidiMessageSequence seq;
        for (int i = 0; i < looper.eventCount (p); ++i)
        {
            const auto& e = looper.event (p, i);
            const double ticks = (double) e.t / samplesPerBeat * ppq;
            seq.addEvent (e.on ? juce::MidiMessage::noteOn  ((p % 16) + 1, e.note, e.vel)
                               : juce::MidiMessage::noteOff ((p % 16) + 1, e.note), ticks);
        }
        seq.updateMatchedPairs();
        mf.addTrack (seq);
    }

    file.deleteFile();
    juce::FileOutputStream os (file);
    return os.openedOk() && mf.writeTo (os);
}

bool VASynthProcessor::exportLoopToWavFile (const juce::File& file) const
{
    // Export = the SUM of all 4 audio lanes (one stereo mix of the recorded loop). Length =
    // the longest lane with content.
    int n = 0;
    for (auto& al : audioLoops) n = juce::jmax (n, al.contentLength());
    if (n <= 0) return false;
    const double sr = getSampleRate() > 0.0 ? getSampleRate() : 48000.0;

    // Sum the lanes, then clamp to [-1,1] so overdub-summed peaks don't wrap in the file.
    juce::AudioBuffer<float> buf (2, n);
    buf.clear();
    for (auto& al : audioLoops)
    {
        const int ln = al.contentLength();
        if (ln <= 0) continue;
        const float* sL = al.dataL(); const float* sR = al.dataR();
        for (int i = 0; i < n; ++i) { buf.addSample (0, i, sL[i % ln]); buf.addSample (1, i, sR[i % ln]); }
    }
    for (int i = 0; i < n; ++i)
    {
        buf.setSample (0, i, juce::jlimit (-1.0f, 1.0f, buf.getSample (0, i)));
        buf.setSample (1, i, juce::jlimit (-1.0f, 1.0f, buf.getSample (1, i)));
    }

    file.deleteFile();
    auto os = file.createOutputStream();
    if (os == nullptr) return false;
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wav.createWriterFor (os.get(), sr, 2, 24, {}, 0));
    if (writer == nullptr) return false;
    os.release();                                   // the writer owns the stream now
    return writer->writeFromAudioSampleBuffer (buf, 0, n);
}

void VASynthProcessor::randomizeMacros (juce::Random& rng)
{
    const auto routable = macroRoutableIDs();
    if (routable.isEmpty()) return;

    // How many macros to assign: 1 always, then 50% / 25% / 10% cumulative for 2 / 3 / 4.
    int count = 1;
    if (rng.nextFloat() < 0.5f) { count = 2; if (rng.nextFloat() < 0.5f) { count = 3; if (rng.nextFloat() < 0.4f) count = 4; } }

    // Distinct macros + distinct target params (Fisher-Yates on both index lists).
    std::array<int, 8> macroIdx { 0, 1, 2, 3, 4, 5, 6, 7 };
    for (int i = 7; i > 0; --i) std::swap (macroIdx[(std::size_t) i], macroIdx[(std::size_t) rng.nextInt (i + 1)]);
    std::vector<int> paramIdx (routable.size());
    for (int i = 0; i < routable.size(); ++i) paramIdx[(std::size_t) i] = i;
    for (int i = routable.size() - 1; i > 0; --i) std::swap (paramIdx[(std::size_t) i], paramIdx[(std::size_t) rng.nextInt (i + 1)]);

    for (auto& t : macroTargetId) t.clear();
    const char* macroIDs[] { ParamID::macro1, ParamID::macro2, ParamID::macro3, ParamID::macro4,
                             ParamID::macro5, ParamID::macro6, ParamID::macro7, ParamID::macro8 };
    for (int k = 0; k < count && k < (int) paramIdx.size(); ++k)
    {
        const int m = macroIdx[(std::size_t) k];
        macroTargetId[(std::size_t) m] = routable[paramIdx[(std::size_t) k]];
        if (auto* p = apvts.getParameter (macroIDs[m])) p->setValueNotifyingHost (rng.nextFloat());
    }
    writeMacroMapProperty();
}

// ---- H5 musical RANDOM + VARY -------------------------------------------------------------
namespace
{
    // Discrete (choice / bool) sound params — VARY flips these rarely, never nudges them.
    bool isDiscreteSoundParam (const juce::String& id)
    {
        namespace ID = ParamID;
        static const juce::StringArray d {
            ID::osc1Wave, ID::osc2Wave, ID::osc3Wave, ID::osc1On, ID::osc2On, ID::osc3On,
            ID::osc1Octave, ID::osc2Octave, ID::osc3Octave, ID::filterType,
            ID::lfoShape, ID::lfoDest, ID::lfo2Shape, ID::lfo2Dest, ID::lfo3Shape, ID::lfo3Dest,
            ID::fxChorusOn, ID::fxDelayOn, ID::fxReverbOn, ID::fxWidthOn, ID::peqOn, ID::polyMode };
        return d.contains (id);
    }

    void setN (juce::AudioProcessorValueTreeState& a, const char* id, float v)
    { if (auto* p = a.getParameter (id)) p->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, v)); }
    float getN (juce::AudioProcessorValueTreeState& a, const char* id)
    { auto* p = a.getParameter (id); return p ? p->getValue() : 0.0f; }

    // WILD: full-range uniform over every non-excluded sound param.
    void applyWildParams (juce::AudioProcessorValueTreeState& a, juce::Random& rng)
    {
        for (auto& id : VASynthProcessor::soundDesignParamIDs())
            if (! PresetManager::randomizeExclusions().contains (id))
                setN (a, id.toRawUTF8(), rng.nextFloat());
    }

    // CONSTRAINED: musical per-param ranges (fewer duds than full uniform).
    void applyConstrainedParams (juce::AudioProcessorValueTreeState& a, juce::Random& rng)
    {
        namespace ID = ParamID;
        for (auto& id : VASynthProcessor::soundDesignParamIDs())
        {
            if (PresetManager::randomizeExclusions().contains (id)) continue;
            float v = rng.nextFloat();
            if      (id == ID::osc1On)        v = 1.0f;
            else if (id == ID::filterCutoff)  v = 0.35f + 0.6f * rng.nextFloat();   // mostly bright enough to read
            else if (id == ID::filterReso)    v = 0.6f  * rng.nextFloat();
            else if (id == ID::lfoDepth || id == ID::lfo2Depth || id == ID::lfo3Depth) v = 0.5f * rng.nextFloat();
            else if (id == ID::noiseLevel)    v = 0.25f * rng.nextFloat();
            else if (id == ID::delayFeedback) v = 0.55f * rng.nextFloat();
            else if (id == ID::ampAttack)     v = 0.5f  * rng.nextFloat();          // rarely a super-slow attack
            else if (id == ID::chorusMix || id == ID::delayMix || id == ID::reverbMix) v = 0.55f * rng.nextFloat();
            setN (a, id.toRawUTF8(), v);
        }
    }

    // ARCHETYPE: a coherent patch within correlated ranges. rr(lo,hi) picks in a range.
    void applyArchetypeParams (juce::AudioProcessorValueTreeState& a, juce::Random& rng, int which)
    {
        namespace ID = ParamID;
        auto rr = [&] (float lo, float hi) { return lo + rng.nextFloat() * (hi - lo); };
        // Sensible shared baseline first, then the archetype overrides its defining params.
        setN (a, ID::osc1On, 1.0f); setN (a, ID::osc2On, rng.nextFloat() < 0.6f ? 1.0f : 0.0f); setN (a, ID::osc3On, 0.0f);
        setN (a, ID::osc1Level, rr (0.6f, 0.9f)); setN (a, ID::osc2Level, rr (0.3f, 0.7f)); setN (a, ID::osc3Level, 0.0f);
        setN (a, ID::noiseLevel, 0.0f); setN (a, ID::filterType, 0.0f);              // LP
        setN (a, ID::filterReso, rr (0.1f, 0.4f)); setN (a, ID::filterKeytrack, rr (0.2f, 0.6f));
        setN (a, ID::ampSustain, rr (0.5f, 0.9f));
        for (const char* id : { ID::chorusMix, ID::delayMix, ID::reverbMix }) setN (a, id, rr (0.1f, 0.35f));

        switch (which)
        {
            case 0: // BASS
                setN (a, ID::osc1Wave, 0.0f); setN (a, ID::osc2Wave, 0.33f);          // saw + square
                setN (a, ID::osc2Octave, 0.25f);                                      // -1 oct sub
                setN (a, ID::filterCutoff, rr (0.2f, 0.45f)); setN (a, ID::filterEnvAmt, rr (0.55f, 0.75f));
                setN (a, ID::ampAttack, rr (0.0f, 0.1f)); setN (a, ID::ampDecay, rr (0.3f, 0.6f)); setN (a, ID::ampSustain, rr (0.3f, 0.6f)); setN (a, ID::ampRelease, rr (0.1f, 0.3f));
                setN (a, ID::fltAttack, 0.0f); setN (a, ID::fltDecay, rr (0.2f, 0.4f)); break;
            case 1: // LEAD
                setN (a, ID::osc1Wave, 0.0f); setN (a, ID::osc2Detune, rr (0.5f, 0.6f));
                setN (a, ID::filterCutoff, rr (0.5f, 0.8f)); setN (a, ID::ampAttack, rr (0.0f, 0.15f)); setN (a, ID::ampRelease, rr (0.2f, 0.4f));
                setN (a, ID::delayMix, rr (0.2f, 0.4f)); break;
            case 2: // PAD
                setN (a, ID::osc1Wave, 0.0f); setN (a, ID::osc2Wave, 0.0f); setN (a, ID::osc2Detune, rr (0.53f, 0.62f));
                setN (a, ID::filterCutoff, rr (0.4f, 0.65f)); setN (a, ID::ampAttack, rr (0.55f, 0.8f)); setN (a, ID::ampRelease, rr (0.6f, 0.85f)); setN (a, ID::ampSustain, rr (0.7f, 0.95f));
                setN (a, ID::reverbMix, rr (0.35f, 0.6f)); setN (a, ID::fxReverbOn, 1.0f); setN (a, ID::chorusMix, rr (0.3f, 0.5f)); setN (a, ID::fxChorusOn, 1.0f); break;
            case 3: // PLUCK
                setN (a, ID::osc1Wave, rng.nextFloat() < 0.5f ? 0.0f : 0.33f);
                setN (a, ID::filterCutoff, rr (0.35f, 0.6f)); setN (a, ID::filterEnvAmt, rr (0.5f, 0.8f));
                setN (a, ID::ampAttack, 0.0f); setN (a, ID::ampDecay, rr (0.2f, 0.4f)); setN (a, ID::ampSustain, rr (0.0f, 0.15f)); setN (a, ID::ampRelease, rr (0.1f, 0.3f));
                setN (a, ID::fltAttack, 0.0f); setN (a, ID::fltDecay, rr (0.15f, 0.35f)); setN (a, ID::delayMix, rr (0.15f, 0.3f)); break;
            case 4: // KEYS / EP
                setN (a, ID::osc1Wave, 0.66f); setN (a, ID::osc2Wave, 1.0f);          // tri + sine
                setN (a, ID::filterCutoff, rr (0.5f, 0.75f)); setN (a, ID::ampAttack, rr (0.0f, 0.1f)); setN (a, ID::ampDecay, rr (0.4f, 0.7f)); setN (a, ID::ampSustain, rr (0.3f, 0.6f));
                setN (a, ID::chorusMix, rr (0.25f, 0.45f)); setN (a, ID::fxChorusOn, 1.0f); break;
            default: // 5: PERC
                setN (a, ID::osc1Wave, 1.0f); setN (a, ID::osc2On, 0.0f); setN (a, ID::noiseLevel, rr (0.2f, 0.5f));
                setN (a, ID::filterCutoff, rr (0.4f, 0.8f)); setN (a, ID::fltEnvToPitch, rr (0.55f, 0.8f));
                setN (a, ID::ampAttack, 0.0f); setN (a, ID::ampDecay, rr (0.1f, 0.3f)); setN (a, ID::ampSustain, 0.0f); setN (a, ID::ampRelease, rr (0.05f, 0.2f));
                setN (a, ID::fltAttack, 0.0f); setN (a, ID::fltDecay, rr (0.05f, 0.2f)); break;
        }
    }

    // AUDIBILITY FLOOR — applied in EVERY mode: never silent, however wild.
    void ensureAudibleParams (juce::AudioProcessorValueTreeState& a)
    {
        namespace ID = ParamID;
        setN (a, ID::osc1On, 1.0f);                                   // >=1 live oscillator...
        setN (a, ID::osc1Level, juce::jmax (0.4f, getN (a, ID::osc1Level)));   // ...at an audible level
        setN (a, ID::ampAttack, juce::jmin (0.7f, getN (a, ID::ampAttack)));   // note actually starts in time
        setN (a, ID::filterCutoff, juce::jmax (0.18f, getN (a, ID::filterCutoff)));   // not fully closed
        if (getN (a, ID::ampSustain) < 0.05f && getN (a, ID::ampDecay) < 0.2f) // silent env -> give it a decay tail
            setN (a, ID::ampDecay, 0.4f);
    }
}

VASynthProcessor::RandomResult VASynthProcessor::randomizeSound (juce::Random& rng)
{
    const int r = rng.nextInt (100);
    const RandomMode m = (r < kRandWildPct)                     ? RandomMode::Wild
                       : (r < kRandWildPct + kRandArchetypePct) ? RandomMode::Archetype
                                                                : RandomMode::Constrained;
    const int arch = (m == RandomMode::Archetype) ? rng.nextInt (kNumArchetypes) : -1;
    return randomizeSound (rng, m, arch);
}

VASynthProcessor::RandomResult VASynthProcessor::randomizeSound (juce::Random& rng, RandomMode mode, int archetype)
{
    RandomResult res; res.mode = mode;
    const int focus = juce::jlimit (0, SynthEngine::maxParts - 1, editFocus());
    auto& mtx = partMatrix[(std::size_t) focus];

    // Curated, musical routes for the tame modes; free-for-all for wild.
    auto addTastefulRoutes = [&] (int n)
    {
        struct R { int src, dest; float depthLo, depthHi; };
        static const R pool[] {
            { ModMatrix::LFO1, ModMatrix::Cutoff,      -0.35f, 0.35f },
            { ModMatrix::ModEnv, ModMatrix::Cutoff,     0.25f, 0.6f  },
            { ModMatrix::LFO2, ModMatrix::PulseWidth,   0.15f, 0.35f },
            { ModMatrix::LFO1, ModMatrix::Pitch,       -0.06f, 0.06f },
            { ModMatrix::Velocity, ModMatrix::Cutoff,   0.2f,  0.5f  },
            { ModMatrix::LFO3, ModMatrix::ReverbMix,    0.1f,  0.3f  } };
        for (int k = 0; k < n; ++k)
        {
            const auto& e = pool[(std::size_t) rng.nextInt ((int) (sizeof pool / sizeof pool[0]))];
            const float d = e.depthLo + rng.nextFloat() * (e.depthHi - e.depthLo);
            linkModRoute (focus, e.src, e.dest, d);
        }
    };
    auto addWildRoutes = [&] (int n)
    {
        const auto& tbl = moddest::table();
        for (int k = 0; k < n; ++k)
        {
            const int src  = 1 + rng.nextInt (ModMatrix::kNumSources - 1);          // any real source
            const int dest = tbl[(std::size_t) rng.nextInt ((int) tbl.size())].dest;
            const float d  = rng.nextFloat() * 2.0f - 1.0f;                          // bipolar
            linkModRoute (focus, src, dest, d);
        }
    };

    for (auto& s : mtx.slots) s = {};                          // start each Random with a clean matrix
    switch (mode)
    {
        case RandomMode::Wild:        applyWildParams (apvts, rng);        addWildRoutes (rng.nextInt (5));      res.label = "WILD"; break;
        case RandomMode::Archetype:   applyArchetypeParams (apvts, rng, archetype); addTastefulRoutes (1 + rng.nextInt (2)); res.label = archetypeName (archetype).toUpperCase() + " archetype"; break;
        default:                      applyConstrainedParams (apvts, rng); addTastefulRoutes (1 + rng.nextInt (2)); res.label = "RANDOM"; break;
    }
    ensureAudibleParams (apvts);
    writeModMatrixProperty();
    return res;
}

void VASynthProcessor::varySound (juce::Random& rng)
{
    for (auto& id : soundDesignParamIDs())
    {
        if (PresetManager::randomizeExclusions().contains (id)) continue;
        auto* p = apvts.getParameter (id);
        if (p == nullptr) continue;
        if (isDiscreteSoundParam (id)) { if (rng.nextFloat() < 0.12f) p->setValueNotifyingHost (rng.nextFloat()); }  // rare flip
        else { const float d = (rng.nextFloat() * 2.0f - 1.0f) * kVaryDelta;
               p->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, p->getValue() + d)); }
    }
    ensureAudibleParams (apvts);
}

juce::String VASynthProcessor::archetypeName (int i)
{
    static const char* names[] { "Bass", "Lead", "Pad", "Pluck", "Keys", "Perc" };
    return (i >= 0 && i < kNumArchetypes) ? names[i] : "Random";
}

void VASynthProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                     juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const auto tStart = std::chrono::steady_clock::now();   // cheap, RT-safe (vDSO)
    buffer.clear();

    const int numSamples = buffer.getNumSamples();
    // No resize on the audio thread — buffers were sized in prepareToPlay.
    // Guard against a misbehaving host sending an oversized block.
    jassert (numSamples <= stereoScratch.getNumSamples());

    // QWERTY notes no longer merge here — they flow through the "QWERTY" surface zones
    // via routeSurfaceMessage() (resolved off the audio thread) and arrive on the routed
    // FIFO, drained below with every other surface. `midi` carries host/DAW events only.

    // Panic (RT-safe): a hot-unplug asked us to release everything.
    if (panicRequested.exchange (false, std::memory_order_acq_rel))
        { engine.allNotesOff(); chordEngine.reset(); midiModMask = 0; lastFedModMask = 0; }

    auto params = snapshotParams();                                     // mutable: block-tier mod may adjust it
    const int editF = editFocusPart.load (std::memory_order_relaxed);   // panel + engine live-param slot
    const int playF = playFocusPart.load (std::memory_order_relaxed);   // which part the LIVE keyboard plays

    // Play-focus hand-off: when the LIVE keyboard moves to a different part, release the
    // notes still sounding on the part we left (else a held note's later note-off routes to
    // the new part and the old voice sticks). Also drop the arp's held set + the chord ledger
    // so nothing replays onto the new part; the momentary chord modifiers stay held.
    if (playF != prevPlayFocus)
    {
        engine.releasePartNotes (prevPlayFocus);
        arp.reset();
        chordEngine.clearHeld();
        prevPlayFocus = playF;
    }

    namespace ID = ParamID;
    const float master   = rp (apvts, ID::masterGain);

    // Per-part LFOs (Sub-phase 2). Part 0 (LIVE) = the panel's three LFOs; locked parts'
    // LFOs travel with their bake (read inside beginMasterBlock), so only part 0 here.
    PartLfos live0Lfo;
    live0Lfo.lfo[0] = { rp (apvts, ID::lfoRate),  rp (apvts, ID::lfoDepth),  (int) rp (apvts, ID::lfoShape),  (int) rp (apvts, ID::lfoDest),  rp (apvts, ID::lfoSync)  > 0.5f, (int) rp (apvts, ID::lfoDiv) };
    live0Lfo.lfo[1] = { rp (apvts, ID::lfo2Rate), rp (apvts, ID::lfo2Depth), (int) rp (apvts, ID::lfo2Shape), (int) rp (apvts, ID::lfo2Dest), rp (apvts, ID::lfo2Sync) > 0.5f, (int) rp (apvts, ID::lfo2Div) };
    live0Lfo.lfo[2] = { rp (apvts, ID::lfo3Rate), rp (apvts, ID::lfo3Depth), (int) rp (apvts, ID::lfo3Shape), (int) rp (apvts, ID::lfo3Dest), rp (apvts, ID::lfo3Sync) > 0.5f, (int) rp (apvts, ID::lfo3Div) };

    // --- chord engine (7B): one played note -> a diatonic chord -------------
    const bool chordOn = rp (apvts, ID::chordEnabled) > 0.5f;
    // Disabling the engine while a chord is held would strand the expanded tones (a later
    // key-up only releases the root), so release the focused part's held tones on the edge.
    if (chordWasOn && ! chordOn) { engine.releasePartNotes (playF); chordEngine.clearHeld(); }
    chordWasOn = chordOn;
    chordEngine.setEnabled (chordOn);
    chordEngine.setRoot  ((int) rp (apvts, ID::chordRoot));
    chordEngine.setScale ((int) rp (apvts, ID::chordScale));
    // Poly/Mono/Legato is PER-PART. The focused part's mode rides in the live apvts;
    // locked parts bake their own (setLockedPartParams), kits are forced poly (engine).
    // Chord mode FORCES the played part poly — a chord in mono would sound only one note.
    // Compute each part's mode ONCE per block (setting it twice would oscillate the mode
    // and re-release the voices every block).
    const int focusMode = (int) rp (apvts, ID::polyMode);
    engine.setPartPolyMode (editF, (chordOn && playF == editF) ? 0 : focusMode);
    if (chordOn && playF != editF) engine.setPartPolyMode (playF, 0);
    // Feed QWERTY modifier edges (message thread) into the latest-wins forcer stack.
    applyChordModifiers (qwertyModMask.load (std::memory_order_acquire) | midiModMask);

    // Master tempo (J1): in a DAW, FOLLOW the host — its BPM drives the arp/seq/looper + synced
    // LFOs, and its play position gives the bar grid the synced LFOs phase-lock to. Standalone (or
    // a host with no tempo) uses the internal Tempo knob. hostBpm/hostPpq set only when valid.
    double hostBpm = 0.0, hostPpq = 0.0; bool hostPlaying = false, haveHostPpq = false;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            if (auto b = pos->getBpm())          hostBpm = *b;
            hostPlaying = pos->getIsPlaying();
            if (auto q = pos->getPpqPosition()) { hostPpq = *q; haveHostPpq = true; }
        }

    // Shared rhythm clock: 16th-note length + swing, common to the arp and sequencer.
    const double bpm = hostBpm >= 20.0 ? hostBpm : (double) juce::jmax (20.0f, rp (apvts, ID::tempo));
    const double sr  = getSampleRate() > 0.0 ? getSampleRate() : 48000.0;
    const double samplesPerStep = juce::jmax (1.0, sr * 60.0 / bpm / 4.0);
    const double samplesPerBeat = samplesPerStep * 4.0;
    const float  swing = rp (apvts, ID::arpSwing);
    publishedBpm.store ((float) bpm, std::memory_order_relaxed);   // UI: Tempo knob shows the effective BPM

    // Always-advancing beat clock for tempo-synced LFOs: snap to the host's play position when it's
    // playing (locks LFOs to the project + its loop brace), else free-run at the effective tempo so a
    // synced LFO oscillates with or without a running transport.
    if (hostPlaying && haveHostPpq) transportBeats = hostPpq;
    else                            transportBeats += (double) numSamples / samplesPerBeat;
    engine.setTransport (transportBeats, samplesPerBeat);

    // --- J3 scene launch: run long-press commands, then engage a pending scene at its quantum ---
    //     (before the seq config + looper block below, so the swapped pattern/clips take effect now).
    serviceSceneCommands (chordOn);
    {
        // Bars are counted on the LOOP clock (which rewinds to 0 on each scene switch), so a
        // quantum lands on the new scene's own grid rather than the free-running song bar.
        const double barLen = juce::jmax (1.0, samplesPerBeat * 4.0);
        const int  bar    = (int) (looper.position() / barLen);
        const bool newBar = sceneClockPrimed && bar != sceneLastBar;   // first block primes (no false boundary)
        sceneLastBar = bar; sceneClockPrimed = true;
        const int pend = pendingScene.load (std::memory_order_acquire);
        // A scene never switches while any part is still recording — a single tap waits until ALL
        // parts (the drum pattern AND every loop, including an in-flight take) have finished.
        bool anyRecording = false;
        for (int L = 0; L < SynthEngine::maxParts; ++L) anyRecording |= loopRecording[(std::size_t) L];
        if (pend >= 0 && pend != activeSceneAudio && ! anyRecording)
        {
            const int q = (int) rp (apvts, ID::sceneQuant);   // 0..3 = 1/2/4/8 bar, 4 = Loop end (default)
            bool boundary = false;
            if (q >= 4)   // Loop end: wait for the LONGEST playing loop to reach its downbeat
            {
                int longest = -1, longestLen = 0;
                for (int L = 0; L < SynthEngine::maxParts; ++L)
                    if (looper.playing (L) && looper.loopLength (L) > longestLen) { longestLen = looper.loopLength (L); longest = L; }
                boundary = (longest >= 0) ? loopLaneWrapped[(std::size_t) longest] : newBar;
            }
            else
            {
                static const int qb[] { 1, 2, 4, 8 };
                const int bars = qb[juce::jlimit (0, 3, q)];
                boundary = newBar && (bar % bars == 0);
            }
            if (boundary) engageSceneFlip (activeSceneAudio, pend, chordOn);
        }
        sceneContentDisp[(std::size_t) activeSceneAudio].store (liveHasContent(), std::memory_order_relaxed);
    }

    // --- arpeggiator (R3, Group 2 = decoupled from the step grid): a pure arpeggiator of
    //     the held notes, playing every 16th at the gate length. HOLD sustains the chord
    //     (arp_latch is retired). OFF => note dispatch is unchanged (bit-identical goldens).
    {
        Arpeggiator::Config ac;
        ac.enabled = rp (apvts, ID::arpOn) > 0.5f;
        ac.mode    = (int) rp (apvts, ID::arpMode);
        ac.octaves = (int) rp (apvts, ID::arpOctaves);
        ac.gate    = rp (apvts, ID::arpGate);
        ac.swing   = swing;
        ac.latch   = rp (apvts, ID::arpHold) > 0.5f;
        ac.samplesPerStep = samplesPerStep;
        // The arp's OWN 16-step grid (independent of the SEQ drum grid): each step is a rest
        // when off, else its own velocity fraction (10..200 %, task #54). Default = all-on 100 %.
        for (int i = 0; i < Arpeggiator::kNumSteps; ++i)
            ac.steps[(std::size_t) i] = (arpSteps[(std::size_t) i] > 0.5f) ? getArpStepVel (i) / 100.0f : 0.0f;
        arp.setConfig (ac);
        if (arpWasOn && ! ac.enabled) { arp.reset(); engine.allNotesOff(); }
        arpWasOn = ac.enabled;
    }

    // --- step sequencer (R3 Group 2): 8-row drum grid on the target part, same clock ---
    {
        StepSequencer::Config sc;
        sc.enabled = rp (apvts, ID::seqOn) > 0.5f;
        sc.gate    = rp (apvts, ID::seqGate);
        sc.swing   = swing;
        sc.samplesPerStep = samplesPerStep;
        sc.cells = seqCells;
        sc.vel   = seqVel;
        sc.note  = seqNotes;
        sc.mute  = seqMutes;
        seq.setConfig (sc);
        // Disabling the sequencer mid-gate must release its held note — the render path
        // below won't call seq.process() once it's off, so flush it here or the voice hangs.
        if (seqWasOn && ! sc.enabled)
        {
            const int seqT = juce::jlimit (0, SynthEngine::maxParts - 1, (int) rp (apvts, ID::seqTarget));
            seq.flush ([this, seqT, chordOn] (int, int note, float, bool) { dispatchNoteOff (note, seqT, chordOn); });
        }
        seqWasOn = sc.enabled;
    }

    // --- looper (R3 Group 3): clock-linked, armed + measure-quantized, dual-lane.
    //     Loop length = bars x bar-length from tempo. REC ARMS; capture engages at the
    //     next loop boundary (a measure) and overdubs each pass while REC stays on. Both
    //     the MIDI lane (note events) and the AUDIO lane (the play-focused part's post-FX,
    //     captured in mixParts) record together; loop_mode picks which lane you HEAR.
    {
        // Per-lane CLEAR requests (bit per lane): wipe that lane's MIDI + audio, flush its notes.
        const int clearMask = loopClearMask.exchange (0, std::memory_order_acq_rel);
        for (int lane = 0; lane < SynthEngine::maxParts; ++lane)
            if (clearMask & (1 << lane)) { looper.clear (lane); audioLoops[(std::size_t) lane].clear(); flushLoopNotesForPart (lane, chordOn); }

        // Loop length tracks the SAME host-aware tempo clock as the arp/seq (bpm/sr from above),
        // so in a DAW the loop grid follows the project tempo (J1) rather than the internal knob.
        const double barSamples = sr * 60.0 / bpm * 4.0;             // 4 beats/bar
        // J2: MASTER wrap = the longest possible loop (32 bars), a common multiple of every lane
        // length so each lane's phase (masterPos % laneLen) is continuous across the master wrap.
        looper.setMasterLength (juce::jmax (1, (int) (barSamples * 32.0)));

        // 1/32-note quantize grid (shared, from tempo): half a 16th step.
        looper.setQuantizeGrid ((int) juce::jmax (1.0, samplesPerStep * 0.5));

        // Per-lane transport param IDs (lane N == part N; lane 1 keeps the original IDs).
        static const char* const recIds[]  { ParamID::loopRec,  ParamID::loopRec2,  ParamID::loopRec3,  ParamID::loopRec4 };
        static const char* const playIds[] { ParamID::loopPlay, ParamID::loopPlay2, ParamID::loopPlay3, ParamID::loopPlay4 };
        static const char* const modeIds[] { ParamID::loopMode, ParamID::loopMode2, ParamID::loopMode3, ParamID::loopMode4 };
        static const char* const barsIds[] { ParamID::loopBars, ParamID::loopBars2, ParamID::loopBars3, ParamID::loopBars4 };
        static const char* const quantIds[]{ ParamID::loopQuant, ParamID::loopQuant2, ParamID::loopQuant3, ParamID::loopQuant4 };
        static const int barsForSel[] { 1, 2, 4, 8, 16, 32 };       // loop_bars choice index -> bars

        for (int lane = 0; lane < SynthEngine::maxParts; ++lane)
        {
            const std::size_t L = (std::size_t) lane;
            auto& al = audioLoops[L];

            // J2: this lane's OWN length. MIDI gets it at any tempo; AUDIO clamps to the ring.
            const int barsSel = juce::jlimit (0, 5, (int) rp (apvts, barsIds[L]));
            const int loopLen = juce::jmax (1, (int) (barSamples * barsForSel[barsSel]));
            looper.setLoopLength (lane, loopLen);
            al.setLoopLength (loopLen);          // clamps to the ring (honest bar-cap lives in the UI)
            loopLenDisp[L].store (loopLen, std::memory_order_relaxed);

            const bool recReq   = rp (apvts, recIds[L])  > 0.5f;
            const bool playReq  = rp (apvts, playIds[L]) > 0.5f;
            const bool audioMode = rp (apvts, modeIds[L]) > 0.5f;    // 0 MIDI re-synth, 1 AUDIO

            if (recReq && ! loopRecPrev[L]) loopArmPending[L] = true;             // REC on -> arm
            if (! recReq)                 { loopArmPending[L] = false; loopRecording[L] = false; }   // REC off -> cancel
            loopRecPrev[L] = recReq;
            // Engage at THIS lane's downbeat: its very start (phase 0) or the block after its wrap.
            if (loopArmPending[L] && (looper.position (lane) == 0 || loopLaneWrapped[L]))
            { loopArmPending[L] = false; loopRecording[L] = true; loopRecJustEngaged[L] = true; }

            // ONE-SHOT fixed length (#47 correction): record exactly one loop (this lane's bars),
            // then auto-stop at the next downbeat and switch this lane to PLAY. The engage block's
            // own wrap is skipped via loopRecJustEngaged so we get one FULL pass.
            if (loopRecording[L] && loopLaneWrapped[L] && ! loopRecJustEngaged[L])
            {
                loopRecording[L] = false;
                if (auto* pr = apvts.getParameter (recIds[L]))  pr->setValueNotifyingHost (0.0f);   // REC off (one-shot)
                if (auto* pp = apvts.getParameter (playIds[L])) pp->setValueNotifyingHost (1.0f);   // auto-play the loop
                loopRecPrev[L] = false;
            }
            loopRecJustEngaged[L] = false;

            looper.setQuantize  (lane, rp (apvts, quantIds[L]) > 0.5f);   // per-lane 1/32 quantize
            looper.setRecording (lane, loopRecording[L]);           // this lane records its own part
            al.setRecording     (loopRecording[L]);
            looper.setPlaying   (lane, playReq && ! audioMode);     // MIDI lane audible for this part
            al.setPlaying       (playReq &&   audioMode);           // AUDIO lane audible
            loopRecStateDisp[L].store (loopRecording[L] ? 2 : (loopArmPending[L] ? 1 : 0), std::memory_order_relaxed);

            // MIDI-lane playback stopped for this lane: release any note it left sounding.
            const bool midiPlayingNow = looper.playing (lane);
            if (loopPlayWasOn[L] && ! midiPlayingNow) flushLoopNotesForPart (lane, chordOn);
            loopPlayWasOn[L] = midiPlayingNow;
        }
    }
    looper.playBlock (numSamples, [this, chordOn] (int part, int note, float vel, bool on)
    {
        if (part >= 0 && part < SynthEngine::maxParts && note >= 0 && note < 128)
            loopNoteHeld[(std::size_t) part][(std::size_t) note] = on;     // track for the stop/clear flush
        if (on) dispatchNoteOn (note, vel, part, chordOn, /*generator*/ true); else dispatchNoteOff (note, part, chordOn);
    });

    // Routed surfaces (per-input MIDI / QWERTY-to-part) — drained at block start, so
    // they sound from sample 0 (block-granular; sample-accurate routing is future).
    drainRoutedMidi (chordOn, playF);

    // Mod matrix (#56): the focused part's live routing table + the current macro values (a
    // matrix source). Locked parts carry their own baked matrix (published with their sound).
    {
        namespace ID = ParamID;
        const char* mids[8] { ID::macro1, ID::macro2, ID::macro3, ID::macro4,
                              ID::macro5, ID::macro6, ID::macro7, ID::macro8 };
        std::array<float, 8> mv {};
        for (int i = 0; i < 8; ++i) mv[(std::size_t) i] = rp (apvts, mids[i]);
        engine.setMacroValues (mv);
        engine.setLiveModMatrix (partMatrix[(std::size_t) juce::jlimit (0, SynthEngine::maxParts - 1, editF)]);
    }

    // Block-tier mod matrix (G4): modulate the focused part's FX/EQ/LFO/env/osc/glide params
    // for THIS block, before they reach the engine. Voice-tier dests (pitch/cutoff/...) are
    // handled per-voice in the engine. No-op (bit-identical) when the matrix is inert.
    FXParams blockFx = snapshotFXParams();
    applyBlockMods (editF, params, blockFx, live0Lfo);

    // --- sample-accurate MIDI dispatch --------------------------------------
    // Multitimbral (Sub-phase 2): each part renders into its OWN buffer (in the engine)
    // so it can run its OWN FX chain; the sum happens once, below. Voices still render
    // per event segment to keep host MIDI note timing tight.
    engine.beginMasterBlock (numSamples, params, blockFx, live0Lfo, editF);

    // Internal generators (arp on the live part + sequencer on its target part) are rendered
    // segmented by their combined, offset-sorted note events. When the arp is on, host notes
    // feed the arp; otherwise host notes are queued as dispatched events at their offsets.
    if (arp.enabled() || seq.enabled())
    {
        const int seqTarget = juce::jlimit (0, SynthEngine::maxParts - 1, (int) rp (apvts, ID::seqTarget));
        // Target changed mid-sequence: release the OLD part's held gate first, else its
        // note-off (dispatched below to the NEW target) never reaches it and the voice hangs.
        if (seqTarget != prevSeqTarget)
        {
            seq.releaseActive ([this, chordOn] (int, int note, float, bool) { dispatchNoteOff (note, prevSeqTarget, chordOn); });
            prevSeqTarget = seqTarget;
        }
        genEvCount = 0;
        // generator = the note came from the seq/arp (yields to live playing when stealing);
        // host notes routed through the gen path are LIVE.
        auto push = [this] (int off, int note, float vel, bool on, int part, bool viaDispatch, bool generator)
        { if (genEvCount < (int) genEv.size()) genEv[(std::size_t) genEvCount++] = { off, note, vel, on, part, viaDispatch, generator }; };

        for (const auto metadata : midi)
        {
            const auto msg = metadata.getMessage();
            const int  pos = metadata.samplePosition;
            if (msg.isNoteOn())
            {
                looper.recordNote (playF, pos, msg.getNoteNumber(), msg.getFloatVelocity(), true);
                if (arp.enabled()) arp.noteOn (msg.getNoteNumber(), msg.getFloatVelocity());
                else               push (pos, msg.getNoteNumber(), msg.getFloatVelocity(), true, playF, true, /*generator*/ false);
            }
            else if (msg.isNoteOff())
            {
                looper.recordNote (playF, pos, msg.getNoteNumber(), 0.0f, false);
                if (arp.enabled()) arp.noteOff (msg.getNoteNumber());
                else               push (pos, msg.getNoteNumber(), 0.0f, false, playF, true, /*generator*/ false);
            }
            else handleControlMessage (msg);
        }

        // Shared transport (task #53): the looper's loop clock is the ONE origin. When this
        // block enters a new BAR (16 sixteenths), re-anchor the arp + sequencer to the
        // downbeat so seq step-1, the arp downbeat, and the looper's boundary coincide (within
        // block tolerance) at every bar — swing still self-accumulates within the bar. This is
        // block-granular and bounds per-bar float drift over a long session.
        {
            const double barLen = 16.0 * samplesPerStep;
            const int barIdxNow = (barLen > 0.0) ? (int) ((double) looper.position() / barLen) : 0;
            if (barIdxNow != prevBarIdx)
            {
                if (arp.enabled()) arp.realign();
                if (seq.enabled()) seq.realign();
                prevBarIdx = barIdxNow;
            }
        }

        if (arp.enabled()) arp.process (numSamples, [&] (int off, int note, float vel, bool on) { push (off, note, vel, on, playF, false, /*generator*/ true); });
        if (seq.enabled()) seq.process (numSamples, [&] (int off, int note, float vel, bool on) { push (off, note, vel, on, seqTarget, true, /*generator*/ true); });

        // Offset-sort (insertion sort — small, RT-safe, stable enough).
        for (int i = 1; i < genEvCount; ++i)
            for (int j = i; j > 0 && genEv[(std::size_t) (j - 1)].offset > genEv[(std::size_t) j].offset; --j)
                std::swap (genEv[(std::size_t) (j - 1)], genEv[(std::size_t) j]);

        int upTo = 0;
        for (int i = 0; i < genEvCount; ++i)
        {
            const auto& ev = genEv[(std::size_t) i];
            const int off = juce::jlimit (0, numSamples, ev.offset);
            if (off > upTo) { engine.renderParts (upTo, off - upTo, params); upTo = off; }
            if (ev.viaDispatch) { if (ev.on) dispatchNoteOn (ev.note, ev.vel, ev.part, chordOn, ev.generator); else dispatchNoteOff (ev.note, ev.part, chordOn); }
            else                { if (ev.on) engine.noteOn (ev.note, ev.vel, ev.part, 0, ev.generator);       else engine.noteOff (ev.note, ev.part); }
        }
        if (upTo < numSamples) engine.renderParts (upTo, numSamples - upTo, params);
        arpStepDisp.store (arp.enabled() ? arp.currentStep() : -1, std::memory_order_relaxed);
        seqStepDisp.store (seq.enabled() ? seq.currentStep() : -1, std::memory_order_relaxed);
    }
    else
    {
        arpStepDisp.store (-1, std::memory_order_relaxed);
        seqStepDisp.store (-1, std::memory_order_relaxed);
        int renderedUpTo = 0;
        for (const auto metadata : midi)
        {
            const auto msg = metadata.getMessage();
            const int  pos = metadata.samplePosition;

            if (pos > renderedUpTo)
            {
                engine.renderParts (renderedUpTo, pos - renderedUpTo, params);
                renderedUpTo = pos;
            }

            if (msg.isNoteOn())                                 // host / QWERTY -> LIVE part 0
            {
                looper.recordNote (playF, pos, msg.getNoteNumber(), msg.getFloatVelocity(), true);
                dispatchNoteOn (msg.getNoteNumber(), msg.getFloatVelocity(), playF, chordOn);
            }
            else if (msg.isNoteOff())
            {
                looper.recordNote (playF, pos, msg.getNoteNumber(), 0.0f, false);
                dispatchNoteOff (msg.getNoteNumber(), playF, chordOn);
            }
            else
                handleControlMessage (msg);                     // CC / pitch-bend / all-off
        }

        if (renderedUpTo < numSamples)
            engine.renderParts (renderedUpTo, numSamples - renderedUpTo, params);
    }

    // Advance the loop clock + mirror its position for the UI. Note whether it wrapped so
    // next block's armed-record can engage on the boundary. The AUDIO lane advances later
    // (after mixParts captures this block), so both lanes stay anchored to the same pos.
    {
        int laneBefore[SynthEngine::maxParts];
        for (int lane = 0; lane < SynthEngine::maxParts; ++lane) laneBefore[lane] = looper.position (lane);
        looper.advance (numSamples);
        for (int lane = 0; lane < SynthEngine::maxParts; ++lane)     // per-lane wrap for next block's engage/one-shot
            loopLaneWrapped[(std::size_t) lane] = looper.position (lane) < laneBefore[lane];
    }
    loopPosDisp.store (looper.position(), std::memory_order_relaxed);

    // Publish the held-modifier mask (QWERTY | MIDI) for the CHORD UI indicators.
    activeModMask.store (qwertyModMask.load (std::memory_order_acquire) | midiModMask,
                         std::memory_order_release);

    // --- per-part FX + master sum (Sub-phase 2). Part 0 (LIVE) uses the panel's FX;
    //     locked/kit parts are dry until their bake includes FX (next increment). The
    //     engine runs each part's own chain and sums to stereo, skipping silent parts.
    float* L = stereoScratch.getWritePointer (0);
    float* R = stereoScratch.getWritePointer (1);
    engine.setMix ({ { rp (apvts, ID::part0Level), rp (apvts, ID::part1Level), rp (apvts, ID::part2Level), rp (apvts, ID::part3Level) } },
                   { { rp (apvts, ID::part0Pan),   rp (apvts, ID::part1Pan),   rp (apvts, ID::part2Pan),   rp (apvts, ID::part3Pan)   } });
    engine.mixParts (L, R, numSamples);                         // per-part FX + level/pan + sum

    // --- AUDIO looper lane (Group 3): in AUDIO mode sum the loop back into the master,
    //     then overdub this block's captured focused-part audio. PLAY *before* RECORD so a
    //     block hears only PRIOR passes (no first-pass doubling) and overdubs layer onto
    //     what was already there. The captured tap comes from mixParts (pre-playback), so
    //     the loop records the live part, never its own playback (no feedback). The MIDI
    //     lane already dispatched at block start. Advance once per block, anchored to pos.
    //     Each of the 4 lanes plays its own AUDIO into the master, then records its OWN
    //     part's post-FX tap (capture-by-lane, NOT by focus — the whole point of #47).
    for (int lane = 0; lane < SynthEngine::maxParts; ++lane)
    {
        // J3: while a scene-audio swap is in flight for this lane, skip it (brief silence) so the
        // message thread can overwrite the ring without a data race — resumes when the swap clears.
        if (audioSwapGuard[(std::size_t) lane].load (std::memory_order_acquire)) continue;
        auto& al = audioLoops[(std::size_t) lane];
        al.playBlock (L, R, numSamples);
        al.recordBlock (engine.capturePartL (lane), engine.capturePartR (lane), numSamples);
        al.advance (numSamples);
    }

    // --- master parametric EQ: RETIRED (K1). The single EQ concept is now the per-part
    //     4-band EQ applied at the end of each part's own chain (FXChain, fixed last
    //     stage). The old global master EQ (eq_* params) is no longer processed here —
    //     its parameters remain registered but inert/hidden for state back-compat, so
    //     old sessions still deserialise without error; they simply have no audible
    //     effect. masterEQ / eqWasOn members are dormant.

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
    pushScope (L, R, numSamples);                 // master scope/FFT tap (RT-safe)

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

    // --- #85: MIDI clock OUT. The synth is an instrument, so its MIDI OUT carries ONLY the clock
    //     (never echoes the input notes). 24-ppq clock + start/stop derived from the SAME transport
    //     as everything else (host tempo in a DAW, else the internal Tempo knob). In a DAW the clock
    //     follows the host play state; standalone runs whenever enabled so external gear (Aeros /
    //     Chase Bliss) locks to the internal tempo. Placed here, after input MIDI has been consumed.
    midi.clear();
    {
        const bool clockEnabled = rp (apvts, ID::clockOut) > 0.5f;
        const bool running = haveHostPpq ? hostPlaying : true;   // DAW: follow transport; standalone: always
        juce::MidiBuffer clockBuf;
        midiClock.process (transportBeats, samplesPerBeat, numSamples, clockEnabled, running,
            [&] (int off, int kind)
            {
                const auto m = kind == MidiClockGenerator::Start ? juce::MidiMessage::midiStart()
                             : kind == MidiClockGenerator::Stop  ? juce::MidiMessage::midiStop()
                                                                 : juce::MidiMessage::midiClock();
                clockBuf.addEvent (m, off);
            });
        if (! clockBuf.isEmpty())
        {
            midi.addEvents (clockBuf, 0, numSamples, 0);         // VST3: to the host's MIDI output
            if (auto* out = clockMidiOut.load (std::memory_order_acquire))
                out->sendBlockOfMessages (clockBuf, juce::Time::getMillisecondCounterHiRes(), getSampleRate());  // standalone device
        }
    }
}

// --- state ---------------------------------------------------------------
void VASynthProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    midiLearn.saveToTree (state);                 // append MIDILEARN child
    modifierLearn.saveToTree (state);             // append MODIFIERLEARN child (7B)

    // Routing lifecycle rule 2: ordinary state persists SOUND only. The multitimbral
    // LAYOUT (locked parts, surface routing, key-range zones) does NOT ride along here —
    // every surface resets to the LIVE patch on relaunch. The layout is recalled ONLY by
    // an explicit MULTI load (see saveMultiState / loadMultiState).

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
        applyMacroMapProperty();       // restore macro->param routing
        applyMacroNamesProperty();     // restore user macro names (H3)
        applyModMatrixProperty();      // restore the per-part mod matrix (#56)
        applyArpStepsProperty();       // restore the arp step pattern
        applySeqProperty();            // restore the sequencer grid

        // Routing lifecycle rule 2: do NOT restore the multitimbral layout from ordinary
        // state — routing/zones reset to default on load. (Only an explicit MULTI load
        // recalls a layout; see loadMultiState.) A stray PARTS child from a pre-rule-2
        // session is intentionally ignored.
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
