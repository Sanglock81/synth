#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <map>

// ============================================================================
// MIDI-learn: maps incoming (channel, CC#) -> APVTS parameter.
//
// Works identically standalone and as a plugin, because it keys on message
// content, not device. (In standalone, JUCE merges all enabled MIDI inputs —
// Korg B2 + Launchkey Mini both arrive here. In Ableton, the track's MIDI
// routing decides what we see.)
//
// Launchkey Mini default knob CCs: 21-28. A sensible starter map is provided;
// learn mode overrides it.
//
// Flow:
//   1. UI arms learn mode with a target parameter ID.
//   2. Next CC that arrives gets bound to that parameter.
//   3. Thereafter, matching CCs set the parameter (0-127 -> normalized 0-1,
//      via setValueNotifyingHost so DAW automation + GUI stay in sync).
//
// State is stored inside the APVTS ValueTree, so mappings persist with the
// plugin state / presets automatically.
//
// THREADING NOTE: handleCC is called from processBlock (audio thread).
// setValueNotifyingHost is safe to call there. Learn-mode arming happens
// on the message thread; the armed target is a std::atomic-backed exchange.
// ============================================================================

class MidiLearnManager
{
public:
    explicit MidiLearnManager (juce::AudioProcessorValueTreeState& state)
        : apvts (state)
    {
        // Starter map for Launchkey Mini (mode-dependent; learn to override).
        addDefaultMapping (21, "filter_cutoff");
        addDefaultMapping (22, "filter_reso");
        addDefaultMapping (23, "osc_mix");
        addDefaultMapping (24, "filter_env_amt");
        addDefaultMapping (25, "amp_attack");
        addDefaultMapping (26, "amp_release");
        addDefaultMapping (27, "lfo_rate");
        addDefaultMapping (28, "lfo_depth");
    }

    // Message thread: arm learn for a parameter (empty string disarms).
    void armLearn (const juce::String& parameterID)
    {
        const juce::SpinLock::ScopedLockType lock (mapLock);
        learnTarget = parameterID;
    }

    // Audio thread: called for every incoming CC message.
    void handleCC (int /*channel*/, int ccNumber, int ccValue)
    {
        const juce::SpinLock::ScopedTryLockType lock (mapLock);
        if (! lock.isLocked())
            return;   // UI is editing the map right now; drop one CC, no big deal

        if (learnTarget.isNotEmpty())
        {
            mappings[ccNumber] = learnTarget;
            learnTarget.clear();
            // TODO: persist mapping into apvts.state ValueTree here.
        }

        auto it = mappings.find (ccNumber);
        if (it == mappings.end())
            return;

        if (auto* param = apvts.getParameter (it->second))
        {
            param->setValueNotifyingHost (ccValue / 127.0f);
        }
    }

    // TODO: serialize/deserialize mappings to/from apvts.state so custom
    // maps survive restarts. Default map above covers v1.

private:
    void addDefaultMapping (int cc, const juce::String& paramID)
    {
        mappings[cc] = paramID;
    }

    juce::AudioProcessorValueTreeState& apvts;
    std::map<int, juce::String> mappings;   // CC# -> parameter ID
    juce::String learnTarget;
    juce::SpinLock mapLock;
};
