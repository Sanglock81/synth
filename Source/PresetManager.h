#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "Parameters.h"
#include "AppInfo.h"

// ============================================================================
// Preset save / load + randomize for sound exploration. Presets are the APVTS
// parameter state written as XML to a per-user config dir; randomize shuffles
// every parameter within its own range (master gain kept audible so you always
// hear the result). JUCE-facing but self-contained; unit-tested via the APVTS.
// ============================================================================

class PresetManager
{
public:
    explicit PresetManager (juce::AudioProcessorValueTreeState& state) : apvts (state) {}

    juce::File presetDir() const { return AppInfo::presetDir(); }

    juce::StringArray getPresetNames() const
    {
        juce::StringArray names;
        for (auto& f : presetDir().findChildFiles (juce::File::findFiles, false, "*.vasynth"))
            names.add (f.getFileNameWithoutExtension());
        names.sortNatural();
        return names;
    }

    // Property name for a user preset's category (additive; presets saved before Inc 3 have
    // none and read back as "User"). Stored on the state tree, stripped again on load so it
    // never pollutes the live parameter state.
    static constexpr const char* kCategoryProp = "vaCategory";

    bool save (const juce::String& name, const juce::String& category = "User") const
    {
        auto safe = juce::File::createLegalFileName (name).trim();
        if (safe.isEmpty()) return false;
        auto state = apvts.copyState();
        PresetPolicy::stripFromState (state);         // don't bake the player's master into the file
        state.setProperty (kCategoryProp, category, nullptr);
        if (auto xml = state.createXml())
            return xml->writeTo (presetDir().getChildFile (safe + ".vasynth"));
        return false;
    }

    // Delete a user preset file. Only user presets live in presetDir(); factory presets are
    // compiled-in and unreachable here, so this can never touch the read-only bank.
    bool remove (const juce::String& name) const
    {
        auto file = presetDir().getChildFile (name + ".vasynth");
        return file.existsAsFile() && file.deleteFile();
    }

    // Rename a user preset, preserving its saved category. Refuses if the target name is empty,
    // unchanged, or already taken (so a rename never silently clobbers another patch).
    bool rename (const juce::String& oldName, const juce::String& newName) const
    {
        auto safe = juce::File::createLegalFileName (newName).trim();
        if (safe.isEmpty() || safe == oldName) return false;
        auto src = presetDir().getChildFile (oldName + ".vasynth");
        auto dst = presetDir().getChildFile (safe + ".vasynth");
        if (! src.existsAsFile() || dst.existsAsFile()) return false;
        return src.moveFileTo (dst);
    }

    // A user preset's saved category ("User" if it predates the field or the file is gone).
    juce::String getPresetCategory (const juce::String& name) const
    {
        auto file = presetDir().getChildFile (name + ".vasynth");
        if (auto xml = juce::XmlDocument::parse (file))
        {
            auto tree = juce::ValueTree::fromXml (*xml);
            auto c = tree.getProperty (kCategoryProp).toString();
            if (c.isNotEmpty()) return c;
        }
        return "User";
    }

    bool load (const juce::String& name)
    {
        auto file = presetDir().getChildFile (name + ".vasynth");
        if (auto xml = juce::XmlDocument::parse (file))
        {
            auto tree = juce::ValueTree::fromXml (*xml);
            tree.removeProperty (kCategoryProp, nullptr);   // menu-only metadata — keep it out of live state
            // Presets the user saved before 6A carry osc_mix but no per-source
            // levels — detect before replaceState back-fills them, then migrate so
            // their existing patches still sound right.
            const bool needsMigration = stateNeedsLevelMigration (tree);
            const auto keep = PresetPolicy::capture (apvts);   // master is the player's, not the preset's
            apvts.replaceState (tree);
            if (needsMigration) applyLegacyOscLevelMigration (apvts);
            PresetPolicy::restore (apvts, keep);
            return true;
        }
        return false;
    }

    // Parameters Randomize must NEVER touch: the ones a player relies on to stay
    // put across patches — performance, global, and routing controls (several of
    // which are commonly mapped to a Launchkey knob). Kept as ONE visible list so
    // the policy is auditable in a glance (and locked in by a test). Everything
    // not on this list is a sound-design parameter and is fair game.
    static const juce::StringArray& randomizeExclusions()
    {
        namespace ID = ParamID;
        static const juce::StringArray excluded {
            ID::masterGain,     // output level (player rides it; often a mapped knob)
            ID::patchTrim,      // per-patch program level (a loudness-match lever, not a sound-design knob)
            ID::velToAmp,       // velocity -> amplitude routing (playing dynamics)
            ID::velToCutoff,    // velocity -> cutoff routing
            ID::polyMode,       // poly / mono / legato performance mode
            ID::glideTime,      // portamento feel
            ID::chordEnabled,   // chord engine config (performance, not sound design)
            ID::chordRoot,
            ID::chordScale,
            ID::oscMix,         // frozen legacy crossfade (engine ignores it)
            // Rhythm section (R3): the arp / sequencer / looper + shared tempo are
            // PERFORMANCE state, not sound design — Random must leave them alone.
            ID::tempo,
            ID::arpOn, ID::arpMode, ID::arpOctaves, ID::arpGate, ID::arpSwing, ID::arpHold,
            ID::loopRec, ID::loopPlay, ID::loopBars, ID::loopMode,
            ID::loopBars2, ID::loopBars3, ID::loopBars4,  // J2: per-lane length is performance state
            ID::sceneQuant,                                // J3: scene launch quantum is performance state
            ID::clockOut                                   // #85: MIDI clock transmit is a routing/global toggle
        };
        // The arp 16-step PATTERN is a state-tree property (not an APVTS parameter), so
        // randomize()'s getParameters() loop never reaches it — the pattern is safe too.
        // Not APVTS parameters, so getParameters() below never reaches them — but
        // recorded here so the exclusion policy is complete: oscillator QUALITY
        // mode (compile-time), the FX chain ORDER (state-tree property), MIDI-learn
        // maps / device profiles, and the (future Phase 7) chord-engine config.
        return excluded;
    }

    // Shuffle only the SOUND-DESIGN parameters (osc waves/levels/detune/PW, filter,
    // envelopes, LFO, FX amounts) named in `soundIds` — i.e. the SELECTED part's sound.
    // Everything else (part mixer levels/pans, master EQ, macros, and every other
    // performance/global/routing control) is left at its current value, so Random
    // reshapes the current patch without disturbing the mix or the other parts. A few
    // ranges are bounded so the result stays musical rather than a wall of noise.
    void randomize (juce::Random& rng, const juce::StringArray& soundIds)
    {
        namespace ID = ParamID;
        for (auto* p : apvts.processor.getParameters())
        {
            auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (p);
            if (withId == nullptr) continue;
            const auto& id = withId->paramID;
            if (! soundIds.contains (id)) continue;                        // sound-design params only
            if (randomizeExclusions().contains (id)) continue;             // ...minus playing-feel (vel/glide)

            float v = rng.nextFloat();                                     // default: full range
            if      (id == ID::osc1On)        v = 1.0f;                     // guarantee a live source
            else if (id == ID::osc1Semi || id == ID::osc2Semi || id == ID::osc3Semi)
                                              v = 0.5f;                     // coarse tune -> 0 st (stay in tune)
            else if (id == ID::lfoDepth)      v = 0.6f  * rng.nextFloat();  // subtle-to-moderate mod
            else if (id == ID::filterReso)    v = 0.7f  * rng.nextFloat();  // avoid a constant scream
            else if (id == ID::noiseLevel)    v = 0.3f  * rng.nextFloat();  // seasoning, not the dish
            else if (id == ID::delayFeedback) v = 0.6f  * rng.nextFloat();  // no runaway repeats
            else if (id == ID::chorusMix || id == ID::delayMix || id == ID::reverbMix)
                                              v = 0.6f  * rng.nextFloat();  // wet, but the note still reads
            p->setValueNotifyingHost (v);
        }
    }

private:
    juce::AudioProcessorValueTreeState& apvts;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetManager)
};
