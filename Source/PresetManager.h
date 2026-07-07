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

    bool save (const juce::String& name) const
    {
        auto safe = juce::File::createLegalFileName (name).trim();
        if (safe.isEmpty()) return false;
        auto state = apvts.copyState();
        PresetPolicy::stripFromState (state);         // don't bake the player's master into the file
        if (auto xml = state.createXml())
            return xml->writeTo (presetDir().getChildFile (safe + ".vasynth"));
        return false;
    }

    bool load (const juce::String& name)
    {
        auto file = presetDir().getChildFile (name + ".vasynth");
        if (auto xml = juce::XmlDocument::parse (file))
        {
            auto tree = juce::ValueTree::fromXml (*xml);
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
            ID::velToAmp,       // velocity -> amplitude routing (playing dynamics)
            ID::velToCutoff,    // velocity -> cutoff routing
            ID::polyMode,       // poly / mono / legato performance mode
            ID::glideTime,      // portamento feel
            ID::oscMix          // frozen legacy crossfade (engine ignores it)
        };
        // Not APVTS parameters, so getParameters() below never reaches them — but
        // recorded here so the exclusion policy is complete: oscillator QUALITY
        // mode (compile-time), the FX chain ORDER (state-tree property), MIDI-learn
        // maps / device profiles, and the (future Phase 7) chord-engine config.
        return excluded;
    }

    // Shuffle only the SOUND-DESIGN parameters (osc waves/levels/detune/PW, filter,
    // envelopes, LFO, FX amounts), leaving every performance/global/routing control
    // at its current value. A few ranges are bounded so the result stays musical
    // rather than a wall of noise or a self-oscillating scream.
    void randomize (juce::Random& rng)
    {
        namespace ID = ParamID;
        for (auto* p : apvts.processor.getParameters())
        {
            auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (p);
            if (withId == nullptr) continue;
            const auto& id = withId->paramID;
            if (randomizeExclusions().contains (id)) continue;             // keep current value

            float v = rng.nextFloat();                                     // default: full range
            if      (id == ID::osc1On)        v = 1.0f;                     // guarantee a live source
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
