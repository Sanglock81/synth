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

    // Where a CC->param mapping came from. Higher wins: a device profile never
    // overwrites a mapping the user learned, and a user profile overrides factory.
    enum class Source { None = 0, Factory = 1, User = 2, Learned = 3 };

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

        for (auto& a : ccToParam) a.store (-1, std::memory_order_relaxed);
        for (auto& a : ccSource)  a.store ((int) Source::None, std::memory_order_relaxed);

        // Built-in factory map (Launchkey Mini pots 21-28). This is the baseline
        // for plugin/DAW use where there's no device hot-plug; the standalone app
        // layers the matched device profile on top. Learn / user profiles override.
        setDefault (21, "filter_cutoff");
        setDefault (22, "filter_reso");
        setDefault (23, "filter_env_amt");
        setDefault (24, "amp_release");
        setDefault (25, "lfo_rate");
        setDefault (26, "lfo_depth");
        setDefault (27, "reverb_mix");
        setDefault (28, "delay_mix");
    }

    // ---- message thread -----------------------------------------------------
    // Arm learn for a parameter (empty string disarms).
    void armLearn (const juce::String& parameterID)
    {
        learnTarget.store (indexOf (parameterID), std::memory_order_release);
    }

    bool isLearning() const { return learnTarget.load (std::memory_order_acquire) >= 0; }

    // Is learn armed for this specific parameter? (UI "armed" visual state.)
    bool isLearningParam (const juce::String& parameterID) const
    {
        const int idx = indexOf (parameterID);
        return idx >= 0 && learnTarget.load (std::memory_order_acquire) == idx;
    }

    // The CC currently bound to a parameter, or -1 if none (UI badge).
    int getCCForParam (const juce::String& parameterID) const
    {
        const int idx = indexOf (parameterID);
        if (idx < 0) return -1;
        for (int cc = 0; cc < numCCs; ++cc)
            if (ccToParam[(std::size_t) cc].load (std::memory_order_acquire) == idx) return cc;
        return -1;
    }

    // Clear every CC mapped to a parameter (UI clear-mapping).
    void clearParam (const juce::String& parameterID)
    {
        const int idx = indexOf (parameterID);
        if (idx < 0) return;
        for (int cc = 0; cc < numCCs; ++cc)
            if (ccToParam[(std::size_t) cc].load (std::memory_order_acquire) == idx)
            {
                ccToParam[(std::size_t) cc].store (-1, std::memory_order_release);
                ccSource[(std::size_t) cc].store ((int) Source::None, std::memory_order_release);
            }
    }

    // ---- device profiles (message thread) -----------------------------------
    // Apply one profile mapping, respecting precedence: it only takes effect if
    // the CC's current mapping is the same source or lower (learned > user >
    // factory). Returns true if it was applied. Used by the profile applier.
    bool applyProfileMapping (int cc, const juce::String& parameterID, Source src)
    {
        if (cc < 0 || cc >= numCCs) return false;
        const int idx = indexOf (parameterID);
        if (idx < 0) return false;
        if (ccSource[(std::size_t) cc].load (std::memory_order_acquire) > (int) src)
            return false;                                   // a higher-precedence mapping holds
        ccToParam[(std::size_t) cc].store (idx, std::memory_order_release);
        ccSource[(std::size_t) cc].store ((int) src, std::memory_order_release);
        return true;
    }

    Source getSource (int cc) const
    {
        if (cc < 0 || cc >= numCCs) return Source::None;
        return (Source) ccSource[(std::size_t) cc].load (std::memory_order_acquire);
    }

    // ---- audio thread: lock-free, allocation-free ---------------------------
    void handleCC (int /*channel*/, int ccNumber, int ccValue)
    {
        if (ccNumber < 0 || ccNumber >= numCCs)
            return;

        // Bind first if learn is armed (consume the arming). One CC per param:
        // clear any CCs already pointing at this param, then bind the new one.
        // (128-entry scan, but only on a user-triggered learn event — still
        // lock-free and allocation-free.)
        const int target = learnTarget.load (std::memory_order_acquire);
        if (target >= 0)
        {
            for (int c = 0; c < numCCs; ++c)
                if (ccToParam[(std::size_t) c].load (std::memory_order_acquire) == target)
                {
                    ccToParam[(std::size_t) c].store (-1, std::memory_order_release);
                    ccSource[(std::size_t) c].store ((int) Source::None, std::memory_order_release);
                }
            ccToParam[(std::size_t) ccNumber].store (target, std::memory_order_release);
            ccSource[(std::size_t) ccNumber].store ((int) Source::Learned, std::memory_order_release);
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
                m.setProperty ("src", ccSource[(size_t) cc].load (std::memory_order_acquire), nullptr);
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

        for (auto& a : ccToParam) a.store (-1, std::memory_order_release);
        for (auto& a : ccSource)  a.store ((int) Source::None, std::memory_order_release);

        for (auto m : ml)
        {
            const int cc  = (int) m.getProperty ("cc", -1);
            const int idx = indexOf (m.getProperty ("param", juce::String()).toString());
            // Back-compat: states saved before source tags treated every mapping
            // as user intent, so default a missing "src" to Learned.
            const int src = (int) m.getProperty ("src", (int) Source::Learned);
            if (cc >= 0 && cc < numCCs && idx >= 0)
            {
                ccToParam[(size_t) cc].store (idx, std::memory_order_release);
                ccSource[(size_t) cc].store (src, std::memory_order_release);
            }
        }
    }

private:
    inline static const juce::Identifier treeType { "MIDILEARN" };

    int indexOf (const juce::String& id) const { return paramIDs.indexOf (id); }

    void setDefault (int cc, const char* id)
    {
        const int i = indexOf (id);
        if (i >= 0)
        {
            ccToParam[(size_t) cc].store (i, std::memory_order_relaxed);
            ccSource[(size_t) cc].store ((int) Source::Factory, std::memory_order_relaxed);
        }
    }

    juce::AudioProcessorValueTreeState& apvts;

    std::vector<juce::AudioProcessorParameter*> paramPtrs;  // index -> param
    juce::StringArray                           paramIDs;   // index -> id (stable key)
    std::array<std::atomic<int>, numCCs>        ccToParam;  // cc -> param index (-1 none)
    std::array<std::atomic<int>, numCCs>        ccSource;   // cc -> Source (precedence)
    std::atomic<int>                            learnTarget { -1 };  // index to bind

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiLearnManager)
};
