#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include <vector>

// ============================================================================
// MIDI-learn: maps incoming CC# -> APVTS parameter. Lock-free and
// allocation-free on the audio thread.
//
// Works identically standalone and as a plugin, because it keys on message
// content, not device. (In standalone, JUCE merges all enabled MIDI inputs —
// Korg B2 + Launchkey Mini both arrive here. In Ableton, the track's MIDI
// routing decides what we see.)
//
// Launchkey Mini default knob CCs: 21-28. A sensible starter map is provided;
// learn mode overrides it.
//
// DESIGN (RT-safety):
//   * ccToParam[cc] is a fixed std::array<std::atomic<int>,128> holding a
//     parameter INDEX (-1 = unmapped). No std::map, no node allocation.
//   * Learn arming crosses the thread boundary via a single std::atomic<int>
//     (the param index to bind, -1 = disarmed). No SpinLock, no juce::String
//     on the audio thread.
//   * handleCC therefore never allocates and never blocks.
//
// Mappings persist inside the APVTS state (see save/loadFromTree), keyed by the
// stable parameter ID string, so custom maps survive restarts and presets.
// ============================================================================

class MidiLearnManager
{
public:
    static constexpr int numCCs = 128;

    explicit MidiLearnManager (juce::AudioProcessorValueTreeState& state)
        : apvts (state)
    {
        // Build the index tables from the processor's parameters (message
        // thread, at construction). index -> (param*, paramID).
        for (auto* p : apvts.processor.getParameters())
            if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (p))
            {
                paramPtrs.push_back (p);
                paramIDs.add (withId->paramID);
            }

        for (auto& a : ccToParam)
            a.store (-1, std::memory_order_relaxed);

        // Starter map for Launchkey Mini (learn to override).
        setDefault (21, "filter_cutoff");
        setDefault (22, "filter_reso");
        setDefault (23, "osc_mix");
        setDefault (24, "filter_env_amt");
        setDefault (25, "amp_attack");
        setDefault (26, "amp_release");
        setDefault (27, "lfo_rate");
        setDefault (28, "lfo_depth");
    }

    // ---- message thread -----------------------------------------------------
    // Arm learn for a parameter (empty string disarms).
    void armLearn (const juce::String& parameterID)
    {
        learnTarget.store (indexOf (parameterID), std::memory_order_release);
    }

    bool isLearning() const { return learnTarget.load (std::memory_order_acquire) >= 0; }

    // ---- audio thread: lock-free, allocation-free ---------------------------
    void handleCC (int /*channel*/, int ccNumber, int ccValue)
    {
        if (ccNumber < 0 || ccNumber >= numCCs)
            return;

        // Bind first if learn is armed (consume the arming).
        const int target = learnTarget.load (std::memory_order_acquire);
        if (target >= 0)
        {
            ccToParam[(size_t) ccNumber].store (target, std::memory_order_release);
            learnTarget.store (-1, std::memory_order_release);
        }

        const int idx = ccToParam[(size_t) ccNumber].load (std::memory_order_acquire);
        if (idx >= 0)
            paramPtrs[(size_t) idx]->setValueNotifyingHost (ccValue / 127.0f);
    }

    // ---- persistence (message thread) ---------------------------------------
    // Writes the current CC->paramID map as a MIDILEARN child of `parent`,
    // replacing any existing one.
    void saveToTree (juce::ValueTree parent) const
    {
        parent.removeChild (parent.getChildWithName (treeType), nullptr);
        juce::ValueTree ml (treeType);
        for (int cc = 0; cc < numCCs; ++cc)
        {
            const int idx = ccToParam[(size_t) cc].load (std::memory_order_acquire);
            if (idx >= 0)
            {
                juce::ValueTree m ("MAP");
                m.setProperty ("cc", cc, nullptr);
                m.setProperty ("param", paramIDs[idx], nullptr);
                ml.addChild (m, -1, nullptr);
            }
        }
        parent.addChild (ml, -1, nullptr);
    }

    // Applies a MIDILEARN child if present. Absent child => keep current map.
    void loadFromTree (const juce::ValueTree& parent)
    {
        auto ml = parent.getChildWithName (treeType);
        if (! ml.isValid())
            return;

        for (auto& a : ccToParam)
            a.store (-1, std::memory_order_release);

        for (auto m : ml)
        {
            const int cc  = (int) m.getProperty ("cc", -1);
            const int idx = indexOf (m.getProperty ("param", juce::String()).toString());
            if (cc >= 0 && cc < numCCs && idx >= 0)
                ccToParam[(size_t) cc].store (idx, std::memory_order_release);
        }
    }

private:
    inline static const juce::Identifier treeType { "MIDILEARN" };

    int indexOf (const juce::String& id) const { return paramIDs.indexOf (id); }

    void setDefault (int cc, const char* id)
    {
        const int i = indexOf (id);
        if (i >= 0) ccToParam[(size_t) cc].store (i, std::memory_order_relaxed);
    }

    juce::AudioProcessorValueTreeState& apvts;

    std::vector<juce::AudioProcessorParameter*> paramPtrs;  // index -> param
    juce::StringArray                           paramIDs;   // index -> id (stable key)
    std::array<std::atomic<int>, numCCs>        ccToParam;  // cc -> param index (-1 none)
    std::atomic<int>                            learnTarget { -1 };  // index to bind

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiLearnManager)
};
