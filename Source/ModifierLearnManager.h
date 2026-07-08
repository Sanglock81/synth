#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "DSP/ChordEngine.h"
#include <array>
#include <atomic>

// ============================================================================
// Modifier-learn: maps an incoming CC (pressed >= 64, footswitch convention) OR a
// note number (pad; the note is CONSUMED, not played) to a momentary chord
// modifier (MAJ/MIN/SUS4/SUS2/DIM/DOM7/7TH). Parallel to MidiLearnManager but a
// separate concern: modifiers aren't APVTS parameters and a source can be a note.
//
// RT-safety mirrors MidiLearnManager exactly: fixed std::array<std::atomic<int>>
// tables (value -> modifier id, -1 = none), a single std::atomic<int> arm word,
// no maps / no allocation / no locks on the audio thread. Mappings persist inside
// the APVTS state as a MODIFIERLEARN child, keyed by the stable modifier NAME.
// ============================================================================

class ModifierLearnManager
{
public:
    static constexpr int kNum = 128;
    static constexpr int kNumModifiers = ChordEngine::kNumModifiers;

    ModifierLearnManager()
    {
        for (auto& a : ccToMod)   a.store (-1, std::memory_order_relaxed);
        for (auto& a : noteToMod) a.store (-1, std::memory_order_relaxed);
    }

    // ---- message thread -----------------------------------------------------
    void armLearn (int modId) { learnTarget.store (modId, std::memory_order_release); }
    void disarm()             { learnTarget.store (-1, std::memory_order_release); }
    bool isLearning() const   { return learnTarget.load (std::memory_order_acquire) >= 0; }
    bool isLearningModifier (int modId) const { return learnTarget.load (std::memory_order_acquire) == modId; }

    // The CC / note currently bound to a modifier, or -1 (for the UI badge).
    int getCCForModifier   (int modId) const { return findSource (ccToMod, modId); }
    int getNoteForModifier (int modId) const { return findSource (noteToMod, modId); }

    void clearModifier (int modId)
    {
        clearSource (ccToMod, modId);
        clearSource (noteToMod, modId);
    }

    // ---- audio thread: lock-free, allocation-free ---------------------------
    // A CC arrived. If learn is armed, a press (>=64) binds this CC. Returns the
    // modifier id this CC controls (and sets `held` from value>=64), or -1.
    int handleCC (int cc, int value, bool& held)
    {
        if (cc < 0 || cc >= kNum) return -1;
        const int t = learnTarget.load (std::memory_order_acquire);
        if (t >= 0 && value >= 64)                       // bind on a press
        {
            bind (ccToMod, cc, t);
            learnTarget.store (-1, std::memory_order_release);
        }
        const int mod = ccToMod[(std::size_t) cc].load (std::memory_order_acquire);
        if (mod >= 0) { held = value >= 64; return mod; }
        return -1;
    }

    // A note-on arrived. If learn is armed, this note binds and is consumed.
    // Returns the modifier id if this note is a modifier source (consume it), or -1.
    int handleNoteOn (int note)
    {
        if (note < 0 || note >= kNum) return -1;
        const int t = learnTarget.load (std::memory_order_acquire);
        if (t >= 0)
        {
            bind (noteToMod, note, t);
            learnTarget.store (-1, std::memory_order_release);
            return t;
        }
        return noteToMod[(std::size_t) note].load (std::memory_order_acquire);
    }

    // A note-off arrived. Returns the modifier id if this note is a source, or -1.
    int handleNoteOff (int note)
    {
        if (note < 0 || note >= kNum) return -1;
        return noteToMod[(std::size_t) note].load (std::memory_order_acquire);
    }

    // ---- persistence (message thread) ---------------------------------------
    void saveToTree (juce::ValueTree parent) const
    {
        parent.removeChild (parent.getChildWithName (treeType), nullptr);
        juce::ValueTree ml (treeType);
        auto emit = [&] (const std::array<std::atomic<int>, kNum>& tbl, const char* kind)
        {
            for (int v = 0; v < kNum; ++v)
            {
                const int mod = tbl[(std::size_t) v].load (std::memory_order_acquire);
                if (mod >= 0)
                {
                    juce::ValueTree m ("MAP");
                    m.setProperty ("kind", kind, nullptr);
                    m.setProperty ("value", v, nullptr);
                    m.setProperty ("mod", ChordEngine::modifierName (mod), nullptr);
                    ml.addChild (m, -1, nullptr);
                }
            }
        };
        emit (ccToMod, "cc");
        emit (noteToMod, "note");
        parent.addChild (ml, -1, nullptr);
    }

    void loadFromTree (const juce::ValueTree& parent)
    {
        auto ml = parent.getChildWithName (treeType);
        if (! ml.isValid()) return;                      // absent -> keep current

        for (auto& a : ccToMod)   a.store (-1, std::memory_order_release);
        for (auto& a : noteToMod) a.store (-1, std::memory_order_release);

        for (auto m : ml)
        {
            const int v   = (int) m.getProperty ("value", -1);
            const int mod = modifierFromName (m.getProperty ("mod", juce::String()).toString());
            const auto kind = m.getProperty ("kind", juce::String()).toString();
            if (v < 0 || v >= kNum || mod < 0) continue;
            (kind == "note" ? noteToMod : ccToMod)[(std::size_t) v].store (mod, std::memory_order_release);
        }
    }

private:
    inline static const juce::Identifier treeType { "MODIFIERLEARN" };

    static int modifierFromName (const juce::String& name)
    {
        for (int i = 0; i < kNumModifiers; ++i)
            if (name == ChordEngine::modifierName (i)) return i;
        return -1;
    }

    // Bind value v to modifier mod in table tbl: first clear any value already
    // pointing at mod (one source per modifier), then set v.
    static void bind (std::array<std::atomic<int>, kNum>& tbl, int v, int mod)
    {
        clearSource (tbl, mod);
        tbl[(std::size_t) v].store (mod, std::memory_order_release);
    }
    static void clearSource (std::array<std::atomic<int>, kNum>& tbl, int mod)
    {
        for (int i = 0; i < kNum; ++i)
            if (tbl[(std::size_t) i].load (std::memory_order_acquire) == mod)
                tbl[(std::size_t) i].store (-1, std::memory_order_release);
    }
    static int findSource (const std::array<std::atomic<int>, kNum>& tbl, int mod)
    {
        for (int i = 0; i < kNum; ++i)
            if (tbl[(std::size_t) i].load (std::memory_order_acquire) == mod) return i;
        return -1;
    }

    std::array<std::atomic<int>, kNum> ccToMod;
    std::array<std::atomic<int>, kNum> noteToMod;
    std::atomic<int> learnTarget { -1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModifierLearnManager)
};
