#pragma once
#include <array>
#include <cstdint>

// ============================================================================
// Diatonic chord engine. Hand-rolled, JUCE-free, allocation-free.
//
// A per-note MIDI preprocessing stage between event intake and voice allocation:
// one played note expands into a diatonic chord, so one finger = a chord that
// stays in key. Out-of-scale notes (with no forcer) pass straight through.
//
// GRAMMAR
//   * In-scale note, no forcer -> the diatonic triad stacked in thirds from that
//     scale degree (root position). So in C major: C->C E G, D->D F A, ...
//   * A momentary FORCER overrides the quality with absolute intervals rooted on
//     the played note (works on any note, in or out of scale):
//       MAJ 0 4 7 | MIN 0 3 7 | SUS4 0 5 7 | SUS2 0 2 7 | DIM 0 3 6 | DOM7 0 4 7 10
//     Conflicting forcers: the latest still-held one wins (a held-order stack).
//   * 7TH adds a seventh: the DIATONIC seventh when no forcer (C->Cmaj7, D->Dm7,
//     G->G7, B->Bm7b5 in C major), otherwise the seventh that fits the forcer
//     (MIN+7TH = min7, MAJ+7TH = maj7, ...). DOM7 always carries its seventh.
//
// LEDGER
//   The exact tones a note produced are recorded at note-on in a fixed-size,
//   note-indexed ledger. note-off replays that ledger entry, so a chord releases
//   precisely the tones it triggered no matter how the modifiers changed while it
//   was held. On a re-press of a held note the tones that changed are released and
//   the rest retrigger (no stuck notes), so the caller feeds engine.noteOn/off.
//
// Chord OFF (disabled) is a bit-identical passthrough: one note in, one note out,
// still ledgered so note-off is symmetric.
// ============================================================================

class ChordEngine
{
public:
    enum Scale { Major = 0, Minor = 1 };
    // Forcers (mutually exclusive, latest-held wins). None = follow the scale.
    enum Forcer { None = 0, Maj, Min, Sus4, Sus2, Dim, Dom7, kNumForcers };

    static constexpr int kMaxTones = 4;     // triad + optional 7th

    // ---- configuration (message/audio thread; plain values) ----------------
    void setEnabled (bool e)   { enabled = e; }
    void setRoot (int pc)      { root = ((pc % 12) + 12) % 12; }
    void setScale (int s)      { scale = (s == Minor) ? Minor : Major; }
    bool isEnabled() const     { return enabled; }

    // A momentary forcer press/release. Maintains a held-order stack so the latest
    // still-pressed forcer is the active one (release it and the prior returns).
    void setForcerHeld (Forcer f, bool held)
    {
        if (f <= None || f >= kNumForcers) return;
        // remove any existing occurrence
        int w = 0;
        for (int i = 0; i < forcerCount; ++i)
            if (forcerStack[(std::size_t) i] != f) forcerStack[(std::size_t) w++] = forcerStack[(std::size_t) i];
        forcerCount = w;
        if (held && forcerCount < (int) forcerStack.size())
            forcerStack[(std::size_t) forcerCount++] = f;
    }
    void setSeventhHeld (bool held) { seventhHeld = held; }

    // Canonical modifier ids — the single bit layout shared by the QWERTY keys, the
    // learned CC/note sources, and the UI. Order is fixed (persisted / bit-packed).
    enum ModifierId { ModMaj = 0, ModMin, ModSus4, ModSus2, ModDim, ModDom7, Mod7th, kNumModifiers };

    void setModifierHeld (int modId, bool held)
    {
        switch (modId)
        {
            case ModMaj:  setForcerHeld (Maj,  held); break;
            case ModMin:  setForcerHeld (Min,  held); break;
            case ModSus4: setForcerHeld (Sus4, held); break;
            case ModSus2: setForcerHeld (Sus2, held); break;
            case ModDim:  setForcerHeld (Dim,  held); break;
            case ModDom7: setForcerHeld (Dom7, held); break;
            case Mod7th:  setSeventhHeld (held);       break;
            default: break;
        }
    }

    static const char* modifierName (int modId)
    {
        static const char* names[kNumModifiers] { "MAJ", "MIN", "SUS4", "SUS2", "DIM", "DOM7", "7TH" };
        return (modId >= 0 && modId < kNumModifiers) ? names[modId] : "";
    }

    Forcer activeForcer() const { return forcerCount > 0 ? forcerStack[(std::size_t) (forcerCount - 1)] : None; }
    bool   seventhActive() const { return seventhHeld; }

    // ---- note events (audio thread; no allocation) -------------------------
    // Expand a played note into its chord. Fills `trigger` with the notes to
    // engine.noteOn (all at `velocity` — caller supplies it) and `release` with any
    // prior tones on this note that are no longer part of the chord (to noteOff
    // first). Returns nothing; counts come back through the out params.
    void noteOn (int note, float velocity, int* trigger, int& nTrigger, int* release, int& nRelease)
    {
        note = clampNote (note);
        int tones[kMaxTones]; int n = buildChord (note, tones);

        // release = old tones (if this note was already sounding) not in the new set
        nRelease = 0;
        auto& e = ledger[(std::size_t) note];
        if (e.active)
            for (int i = 0; i < e.count; ++i)
                if (! contains (tones, n, e.tones[i]))
                    release[nRelease++] = e.tones[i];

        // record + emit the new tones
        nTrigger = n;
        for (int i = 0; i < n; ++i) trigger[i] = tones[i];
        e.active = true; e.count = n; e.vel = velocity;   // remember velocity for re-voicing (1.4)
        for (int i = 0; i < n; ++i) e.tones[i] = tones[i];
    }

    // 1.4: re-voice every HELD chord for the current modifier state (call after a modifier
    // press/release edge). Per held trigger note, diff old vs new tones: releases removed
    // tones and triggers added tones at the note's stored velocity; unchanged tones keep
    // sounding (no retrigger click). Updates the ledger. RT-safe (no allocation).
    //   release(int note);  trigger(int note, float velocity)
    template <typename Release, typename Trigger>
    void revoiceHeld (Release&& release, Trigger&& trigger)
    {
        if (! enabled) return;
        for (int played = 0; played < 128; ++played)
        {
            auto& e = ledger[(std::size_t) played];
            if (! e.active) continue;
            int tones[kMaxTones]; const int n = buildChord (played, tones);
            for (int i = 0; i < e.count; ++i) if (! contains (tones, n, e.tones[i]))       release (e.tones[i]);
            for (int i = 0; i < n; ++i)       if (! contains (e.tones, e.count, tones[i])) trigger (tones[i], e.vel);
            e.count = n; for (int i = 0; i < n; ++i) e.tones[i] = tones[i];
        }
    }

    // Release exactly the tones this note produced; clears the ledger entry.
    void noteOff (int note, int* release, int& nRelease)
    {
        note = clampNote (note);
        auto& e = ledger[(std::size_t) note];
        nRelease = 0;
        if (! e.active) { release[nRelease++] = note; return; }   // safety: release the note itself
        for (int i = 0; i < e.count; ++i) release[nRelease++] = e.tones[i];
        e.active = false; e.count = 0;
    }

    // Forget held chords WITHOUT touching the momentary forcer stack (1.3: on an edit-focus
    // hand-off the caller releases the old part's voices; drop the ledger so a later note-off
    // doesn't replay tones onto the new part, but keep any still-held modifier keys).
    void clearHeld() { for (auto& e : ledger) { e.active = false; e.count = 0; } }

    // Forget all held chords (e.g. on disable/panic). The caller is expected to
    // have released voices separately (allNotesOff).
    void reset()
    {
        for (auto& e : ledger) { e.active = false; e.count = 0; }
        forcerCount = 0; seventhHeld = false;
    }

    // Max extra tones a single note can create (for caller buffer sizing).
    static constexpr int maxTonesPerNote() { return kMaxTones; }

private:
    struct Entry { bool active = false; int count = 0; int tones[kMaxTones] {}; float vel = 0.8f; };

    static int clampNote (int n) { return n < 0 ? 0 : (n > 127 ? 127 : n); }
    static bool contains (const int* a, int n, int v) { for (int i = 0; i < n; ++i) if (a[i] == v) return true; return false; }

    const int* scaleTable() const
    {
        static constexpr int major[7] { 0, 2, 4, 5, 7, 9, 11 };
        static constexpr int minor[7] { 0, 2, 3, 5, 7, 8, 10 };
        return scale == Minor ? minor : major;
    }
    // Semitone offset of scale degree d (d may exceed 6 -> wraps octaves).
    int extScale (int d) const { return 12 * (d / 7) + scaleTable()[(std::size_t) (d % 7)]; }

    // Add `v` to out[] if in range and not already present. Returns updated count.
    static int addTone (int* out, int count, int v)
    {
        if (v < 0 || v > 127) return count;
        for (int i = 0; i < count; ++i) if (out[i] == v) return count;
        out[count] = v; return count + 1;
    }

    // Build the chord for a played note into out[kMaxTones]; returns the count.
    int buildChord (int note, int* out) const
    {
        if (! enabled) { out[0] = note; return 1; }

        const Forcer f = activeForcer();
        int count = 0;

        if (f == None)
        {
            // Diatonic: find the played note's scale degree.
            const int pc = (((note - root) % 12) + 12) % 12;
            int deg = -1;
            const int* sc = scaleTable();
            for (int k = 0; k < 7; ++k) if (sc[k] == pc) { deg = k; break; }

            if (deg < 0) { out[0] = note; return 1; }   // out of scale -> single note

            count = addTone (out, count, note);                              // root (degree)
            count = addTone (out, count, note + extScale (deg + 2) - sc[deg]);// third
            count = addTone (out, count, note + extScale (deg + 4) - sc[deg]);// fifth
            if (seventhHeld)
                count = addTone (out, count, note + extScale (deg + 6) - sc[deg]);
        }
        else
        {
            // Forced absolute-interval chord rooted on the played note.
            static constexpr int tri[kNumForcers][3] {
                {0,0,0},        // None (unused)
                {0,4,7},        // Maj
                {0,3,7},        // Min
                {0,5,7},        // Sus4
                {0,2,7},        // Sus2
                {0,3,6},        // Dim
                {0,4,7},        // Dom7
            };
            static constexpr int seventh[kNumForcers] { 0, 11, 10, 10, 10, 9, 10 };
            for (int i = 0; i < 3; ++i) count = addTone (out, count, note + tri[f][i]);
            if (seventhHeld || f == Dom7)
                count = addTone (out, count, note + seventh[f]);
        }
        return count;
    }

    bool  enabled = false;
    int   root = 0;                 // pitch class 0..11
    int   scale = Major;

    std::array<Forcer, kNumForcers> forcerStack {};
    int   forcerCount = 0;
    bool  seventhHeld = false;

    std::array<Entry, 128> ledger {};   // per played-note tone record
};
