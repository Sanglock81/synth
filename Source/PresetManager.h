#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

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

    juce::File presetDir() const
    {
        auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                       .getChildFile ("VASynth").getChildFile ("presets");
        dir.createDirectory();
        return dir;
    }

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
        if (auto xml = apvts.copyState().createXml())
            return xml->writeTo (presetDir().getChildFile (safe + ".vasynth"));
        return false;
    }

    bool load (const juce::String& name)
    {
        auto file = presetDir().getChildFile (name + ".vasynth");
        if (auto xml = juce::XmlDocument::parse (file))
        {
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
            return true;
        }
        return false;
    }

    // Shuffle every parameter within its range. Master gain is constrained to an
    // audible band so a random patch is never silent from gain alone.
    void randomize (juce::Random& rng)
    {
        for (auto* p : apvts.processor.getParameters())
        {
            if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (p))
            {
                float v = rng.nextFloat();
                if (withId->paramID == "master_gain") v = 0.5f + 0.3f * rng.nextFloat();  // 0.5..0.8
                p->setValueNotifyingHost (v);
            }
        }
    }

private:
    juce::AudioProcessorValueTreeState& apvts;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetManager)
};
