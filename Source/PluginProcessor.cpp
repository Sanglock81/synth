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

    applyArpStepsProperty();      // seed the 16-step pattern with its default (all on)
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
    engine.prepare (sampleRate, juce::jmax (1, samplesPerBlock));   // sizes the per-part FX buffers too
    // Allocate the stereo master buffer ONCE, at the host's max block size. processBlock
    // never resizes it (JUCE guarantees numSamples <= this). Per-part mix buffers + FX
    // live in the engine now (Sub-phase 2).
    stereoScratch.setSize (2, juce::jmax (1, samplesPerBlock), false, false, true);

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
    p.enabled[FXChain::Chorus_] = rp (src, ID::fxChorusOn) > 0.5f;
    p.enabled[FXChain::Delay_]  = rp (src, ID::fxDelayOn)  > 0.5f;
    p.enabled[FXChain::Reverb_] = rp (src, ID::fxReverbOn) > 0.5f;
    p.enabled[FXChain::Width_]  = rp (src, ID::fxWidthOn)  > 0.5f;
    auto toks = juce::StringArray::fromTokens (src.state.getProperty (ID::fxOrder).toString(), ",", "");
    if (toks.size() == 4)
    {
        int o[4]; bool seen[4] = {}; bool okOrder = true;
        for (int i = 0; i < 4; ++i) { o[i] = toks[i].getIntValue(); if (o[i] < 0 || o[i] > 3 || seen[o[i]]) okOrder = false; else seen[o[i]] = true; }
        if (okOrder) for (int i = 0; i < 4; ++i) p.order[i] = o[i];
    }
    return p;
}

// Read the three LFOs from any APVTS (for baking a locked part's modulation).
static PartLfos lfosFrom (const juce::AudioProcessorValueTreeState& src)
{
    namespace ID = ParamID; PartLfos pl;
    pl.lfo[0] = { rp (src, ID::lfoRate),  rp (src, ID::lfoDepth),  (int) rp (src, ID::lfoShape),  (int) rp (src, ID::lfoDest) };
    pl.lfo[1] = { rp (src, ID::lfo2Rate), rp (src, ID::lfo2Depth), (int) rp (src, ID::lfo2Shape), (int) rp (src, ID::lfo2Dest) };
    pl.lfo[2] = { rp (src, ID::lfo3Rate), rp (src, ID::lfo3Depth), (int) rp (src, ID::lfo3Shape), (int) rp (src, ID::lfo3Dest) };
    return pl;
}

// Bake a source preset (factory / user / Init) into VoiceParams via the scratch APVTS,
// reusing the exact kill-fold so a baked part is bit-identical to loading it live.
// `ok` is false if a named preset was missing (Init baked instead). Optionally also
// extracts the preset's FX + LFO (for a locked part; kit pads pass null). Shared by
// locked parts and kit pads.
VoiceParams VASynthProcessor::bakePresetParams (const juce::String& name, bool& ok,
                                                FXParams* fxOut, PartLfos* lfoOut)
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
    if (fxOut  != nullptr) *fxOut  = fxParamsFrom (scratch.apvts);
    if (lfoOut != nullptr) *lfoOut = lfosFrom (scratch.apvts);
    return buildVoiceParams (scratch.apvts);
}

bool VASynthProcessor::setPartPreset (int part, const juce::String& name)
{
    if (part < 1 || part >= SynthEngine::maxParts) return false;

    bool ok = true;
    FXParams fx; PartLfos lfo;
    const VoiceParams vp = bakePresetParams (name, ok, &fx, &lfo);
    engine.setLockedPartParams (part, vp, fx, lfo);  // voice + FX + LFOs published together
    engine.clearPartKit (part);                     // a plain preset turns any kit off
    partKits[(std::size_t) part] = KitDefinition{}; // forget the kit definition
    partPresetName[(std::size_t) part] = ok ? name : juce::String ("Init");
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

        bool ok = true;
        auto vp = bakePresetParams (pd.source, ok);
        vp.gain *= juce::jlimit (0.0f, 4.0f, pd.level);               // fold pad level into the voice
        kd.params[(std::size_t) i] = vp;
        out.triggerNote = pd.triggerNote;
        out.numSound    = juce::jlimit (1, kMaxPadSoundNotes, pd.numSound);
        for (int s = 0; s < kMaxPadSoundNotes; ++s) out.soundNote[(std::size_t) s] = pd.soundNote[(std::size_t) s];
        out.chokeGroup  = juce::jlimit (0, 8, pd.chokeGroup);
    }
    engine.setPartKit (part, kd);                    // a Kit part is dry in v1 (engine uses dry FX/LFO for kits)
    partKits[(std::size_t) part] = def;
    partPresetName[(std::size_t) part] = def.name.isNotEmpty() ? def.name : juce::String ("Kit");
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
        p.setProperty ("num",     pd.numSound, nullptr);
        p.setProperty ("level",   pd.level, nullptr);
        p.setProperty ("choke",   pd.chokeGroup, nullptr);
        for (int s = 0; s < 4; ++s) p.setProperty ("n" + juce::String (s), pd.soundNote[(std::size_t) s], nullptr);
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
        pd.numSound    = (int) p.getProperty ("num", 1);
        pd.level       = (float) p.getProperty ("level", 1.0);
        pd.chokeGroup  = (int) p.getProperty ("choke", 0);
        for (int s = 0; s < 4; ++s) pd.soundNote[(std::size_t) s] = (int) p.getProperty ("n" + juce::String (s), 60);
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
        routeMidi (m, 0);                            // CC / pitch-bend / etc: global / live part
}

// ---- MULTI layouts: a named snapshot of parts + surface zones ---------------

juce::ValueTree VASynthProcessor::captureMultiState() const
{
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
                partOk[(std::size_t) p] = setPartPreset (p, e.getProperty ("preset", juce::String()).toString());
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
    else if (engine.partIsKit (part))
    {
        partLastTrig[(std::size_t) part].store (note, std::memory_order_relaxed);   // pad flicker
        engine.kitNoteOn (part, note, vel);           // trigger -> pad (sounding notes + choke)
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
    else if (engine.partIsKit (part))
        engine.kitNoteOff (part, note);
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
        // When the arp is on, LIVE-part (0) played notes feed the arp instead of sounding
        // directly; it re-emits them on its clock. Locked parts + control are unaffected.
        const bool arpCapture = arp.enabled() && e.part == 0;
        if (m.isNoteOn())       { if (arpCapture) arp.noteOn (e.d1, e.d2 / 127.0f); else dispatchNoteOn  (e.d1, e.d2 / 127.0f, e.part, chordOn); }
        else if (m.isNoteOff()) { if (arpCapture) arp.noteOff (e.d1);               else dispatchNoteOff (e.d1, e.part, chordOn); }
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

void VASynthProcessor::applyMacroMapProperty()
{
    for (auto& t : macroTargetId) t.clear();
    const auto str = apvts.state.getProperty ("macro_map").toString();
    if (str.isEmpty()) return;
    auto tokens = juce::StringArray::fromTokens (str, ",", "");
    for (int i = 0; i < juce::jmin (8, tokens.size()); ++i)
        if (tokens[i] != "-" && apvts.getParameter (tokens[i]) != nullptr)
            macroTargetId[(std::size_t) i] = tokens[i];
}

void VASynthProcessor::writeArpStepsProperty()
{
    juce::StringArray v;
    for (float s : arpSteps) v.add (juce::String (s, 3));
    apvts.state.setProperty ("arp_steps", v.joinIntoString (","), nullptr);
}

void VASynthProcessor::applyArpStepsProperty()
{
    const auto str = apvts.state.getProperty ("arp_steps").toString();
    auto tokens = juce::StringArray::fromTokens (str, ",", "");
    for (int i = 0; i < kArpSteps; ++i)
        arpSteps[(std::size_t) i] = (i < tokens.size()) ? juce::jlimit (0.0f, 1.0f, tokens[i].getFloatValue())
                                                        : 0.8f;   // sensible default: all steps on
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

    const auto params = snapshotParams();

    namespace ID = ParamID;
    const float master   = rp (apvts, ID::masterGain);

    // Per-part LFOs (Sub-phase 2). Part 0 (LIVE) = the panel's three LFOs; locked parts'
    // LFOs travel with their bake (read inside beginMasterBlock), so only part 0 here.
    PartLfos live0Lfo;
    live0Lfo.lfo[0] = { rp (apvts, ID::lfoRate),  rp (apvts, ID::lfoDepth),  (int) rp (apvts, ID::lfoShape),  (int) rp (apvts, ID::lfoDest) };
    live0Lfo.lfo[1] = { rp (apvts, ID::lfo2Rate), rp (apvts, ID::lfo2Depth), (int) rp (apvts, ID::lfo2Shape), (int) rp (apvts, ID::lfo2Dest) };
    live0Lfo.lfo[2] = { rp (apvts, ID::lfo3Rate), rp (apvts, ID::lfo3Depth), (int) rp (apvts, ID::lfo3Shape), (int) rp (apvts, ID::lfo3Dest) };

    // --- chord engine (7B): one played note -> a diatonic chord -------------
    const bool chordOn = rp (apvts, ID::chordEnabled) > 0.5f;
    chordEngine.setEnabled (chordOn);
    chordEngine.setRoot  ((int) rp (apvts, ID::chordRoot));
    chordEngine.setScale ((int) rp (apvts, ID::chordScale));
    // Chord mode FORCES poly — a chord in mono/legato would sound only one note.
    engine.setPolyMode (chordOn ? 0 : (int) rp (apvts, ID::polyMode));
    // Feed QWERTY modifier edges (message thread) into the latest-wins forcer stack.
    applyChordModifiers (qwertyModMask.load (std::memory_order_acquire) | midiModMask);

    // --- arpeggiator (R3): configure from params + tempo. When ON, it captures the LIVE
    //     part's played notes (in drainRoutedMidi + the host loop below) and re-emits them
    //     on its internal clock. OFF => note dispatch is unchanged (bit-identical goldens).
    {
        Arpeggiator::Config ac;
        ac.enabled = rp (apvts, ID::arpOn) > 0.5f;
        ac.mode    = (int) rp (apvts, ID::arpMode);
        ac.octaves = (int) rp (apvts, ID::arpOctaves);
        ac.gate    = rp (apvts, ID::arpGate);
        ac.swing   = rp (apvts, ID::arpSwing);
        ac.latch   = (rp (apvts, ID::arpLatch) > 0.5f) || (rp (apvts, ID::arpHold) > 0.5f);
        const double bpm = juce::jmax (20.0f, rp (apvts, ID::tempo));
        const double sr  = getSampleRate() > 0.0 ? getSampleRate() : 48000.0;
        ac.samplesPerStep = juce::jmax (1.0, sr * 60.0 / bpm / 4.0);        // 16th-note steps
        for (int i = 0; i < kArpSteps; ++i) ac.steps[(std::size_t) i] = arpSteps[(std::size_t) i];
        arp.setConfig (ac);

        // On an arp ON->OFF toggle, clear the arp and release any note it left sounding.
        if (arpWasOn && ! ac.enabled) { arp.reset(); engine.allNotesOff(); }
        arpWasOn = ac.enabled;
    }

    // Routed surfaces (per-input MIDI / QWERTY-to-part) — drained at block start, so
    // they sound from sample 0 (block-granular; sample-accurate routing is future).
    drainRoutedMidi (chordOn);

    // --- sample-accurate MIDI dispatch --------------------------------------
    // Multitimbral (Sub-phase 2): each part renders into its OWN buffer (in the engine)
    // so it can run its OWN FX chain; the sum happens once, below. Voices still render
    // per event segment to keep host MIDI note timing tight.
    engine.beginMasterBlock (numSamples, params, snapshotFXParams(), live0Lfo);

    if (arp.enabled())
    {
        // Host/DAW notes feed the arp (block-granular input is fine — the arp re-times its
        // output); control applies immediately. Then generate clock-timed arp events and
        // render the block segmented by THOSE.
        for (const auto metadata : midi)
        {
            const auto msg = metadata.getMessage();
            if      (msg.isNoteOn())  arp.noteOn (msg.getNoteNumber(), msg.getFloatVelocity());
            else if (msg.isNoteOff()) arp.noteOff (msg.getNoteNumber());
            else                      handleControlMessage (msg);
        }

        arpEvCount = 0;
        arp.process (numSamples, [this] (int off, int note, float vel, bool on)
        {
            if (arpEvCount < (int) arpEv.size()) arpEv[(std::size_t) arpEvCount++] = { off, note, vel, on };
        });

        int upTo = 0;
        for (int i = 0; i < arpEvCount; ++i)
        {
            const auto& ev = arpEv[(std::size_t) i];
            const int off = juce::jlimit (0, numSamples, ev.offset);
            if (off > upTo) { engine.renderParts (upTo, off - upTo, params); upTo = off; }
            if (ev.on) engine.noteOn (ev.note, ev.vel, 0); else engine.noteOff (ev.note, 0);
        }
        if (upTo < numSamples) engine.renderParts (upTo, numSamples - upTo, params);
        arpStepDisp.store (arp.currentStep(), std::memory_order_relaxed);   // playhead for the UI
    }
    else
    {
        arpStepDisp.store (-1, std::memory_order_relaxed);
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
                dispatchNoteOn (msg.getNoteNumber(), msg.getFloatVelocity(), 0, chordOn);
            else if (msg.isNoteOff())
                dispatchNoteOff (msg.getNoteNumber(), 0, chordOn);
            else
                handleControlMessage (msg);                     // CC / pitch-bend / all-off
        }

        if (renderedUpTo < numSamples)
            engine.renderParts (renderedUpTo, numSamples - renderedUpTo, params);
    }

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

    // --- master parametric EQ (end of chain: post-FX sum, pre master gain). Off/flat
    //     is a true bypass (skipped), so the output is bit-identical until it's used.
    const bool eqOn = rp (apvts, ID::eqOn) > 0.5f;
    if (eqOn)
    {
        if (! eqWasOn) masterEQ.reset();                        // clean state on off->on
        masterEQ.setBands ({ rp (apvts, ID::eqLsFreq), rp (apvts, ID::eqLsGain), 0.7f },
                           { rp (apvts, ID::eqLmFreq), rp (apvts, ID::eqLmGain), rp (apvts, ID::eqLmQ) },
                           { rp (apvts, ID::eqHmFreq), rp (apvts, ID::eqHmGain), rp (apvts, ID::eqHmQ) },
                           { rp (apvts, ID::eqHsFreq), rp (apvts, ID::eqHsGain), 0.7f });
        masterEQ.process (L, R, numSamples);
    }
    eqWasOn = eqOn;

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
        applyArpStepsProperty();       // restore the arp step pattern

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
