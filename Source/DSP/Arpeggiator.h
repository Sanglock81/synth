#pragma once
#include <array>
#include <algorithm>
#include <cstdint>

// ============================================================================
// Arpeggiator + 16-step gate — JUCE-free, RT-safe (fixed storage, no alloc/lock).
//
// Held notes form a chord; the arp steps through them on an internal clock in the
// chosen order (Up / Down / Up-Down / Random / As-played), spread over `octaves`,
// with a per-step velocity pattern (0 = rest). GATE sets note length, SWING delays
// the off-beat 16ths, LATCH/HOLD sustain the chord after the keys are released.
//
// The owner (the processor) feeds held-note edges via noteOn/noteOff (block-granular
// is fine — the arp re-times output to its clock) and calls process() once per block
// to emit clock-accurate note events (sample offsets) through a callback.
// ============================================================================

class Arpeggiator
{
public:
    enum Mode { Up = 0, Down, UpDown, Random, AsPlayed };
    static constexpr int kMaxHeld  = 16;
    static constexpr int kNumSteps = 16;

    struct Config
    {
        bool   enabled  = false;
        bool   latch    = false;      // sustain the chord after release (LATCH or HOLD)
        int    mode     = Up;
        int    octaves  = 1;          // 1..4
        float  gate     = 0.5f;       // 0..1 of a step
        float  swing    = 0.0f;       // 0..~0.7 (delays odd 16ths)
        double samplesPerStep = 6000; // 16th-note length in samples (from tempo)
        std::array<float, kNumSteps> steps { };   // per-step velocity 0..1 (0 = rest)
    };

    void setConfig (const Config& c) { cfg = c; }
    bool enabled() const { return cfg.enabled; }
    int  currentStep() const { return stepIndex; }        // -1 = idle; for the UI playhead

    void reset()
    {
        heldCount = keysDown = 0;
        activeNote = -1;
        stepIndex = -1;
        sampleInStep = 0.0;
        seqCursor = 0;
        started = false;
    }

    // A held-note edge (from the played surface). Block-granular timing is fine.
    void noteOn (int note, float vel)
    {
        if (cfg.latch && keysDown == 0) heldCount = 0;   // new physical chord starts fresh
        ++keysDown;
        addHeld (note, vel);
    }
    void noteOff (int note)
    {
        if (keysDown > 0) --keysDown;
        if (! cfg.latch) removeHeld (note);
    }

    // Re-anchor to the shared transport downbeat (task #53): the next process() fires the
    // next pattern note at offset 0 (phase reset; seqCursor/pattern position preserved).
    // The owner calls this at each bar boundary so the arp downbeat coincides with the
    // sequencer step-1 + the looper boundary. process()/swing/gate logic untouched.
    void realign() { started = false; sampleInStep = 0.0; }

    // Emit clock-accurate events for this block. emit(sampleOffset, note, velocity, isOn).
    template <typename Emit>
    void process (int numSamples, Emit&& emit)
    {
        if (! cfg.enabled) return;

        if (heldCount == 0)                              // nothing held -> stop + rest the clock
        {
            if (activeNote >= 0) { emit (0, activeNote, 0.0f, false); activeNote = -1; }
            stepIndex = -1; sampleInStep = 0.0; seqCursor = 0; started = false;
            return;
        }

        double pos = 0.0;
        if (! started) { started = true; stepIndex = -1; sampleInStep = 0.0; doStep (0, emit); }

        const double eps = 1.0e-6;
        while (pos < numSamples)
        {
            const double stepLen = stepLength();
            const double toStep = stepLen - sampleInStep;
            const double toGate = (activeNote >= 0) ? gateRemaining : 1.0e18;
            const double adv = std::min ({ toStep, toGate, (double) numSamples - pos });

            pos += adv; sampleInStep += adv;
            if (activeNote >= 0) gateRemaining -= adv;

            bool acted = false;
            if (activeNote >= 0 && gateRemaining <= eps)
            { emit ((int) pos, activeNote, 0.0f, false); activeNote = -1; acted = true; }

            if (sampleInStep >= stepLen - eps)
            { sampleInStep -= stepLen; doStep ((int) pos, emit); acted = true; }

            if (! acted) break;                          // consumed the rest of the block
        }
    }

private:
    double stepLength() const
    {
        const double s = (double) cfg.swing;             // odd 16ths shortened, evens lengthened
        return cfg.samplesPerStep * ((stepIndex % 2 == 0) ? (1.0 + s) : (1.0 - s));
    }

    template <typename Emit>
    void doStep (int pos, Emit&& emit)
    {
        if (activeNote >= 0) { emit (pos, activeNote, 0.0f, false); activeNote = -1; }
        stepIndex = (stepIndex + 1) % kNumSteps;
        const float sv = cfg.steps[(std::size_t) stepIndex];
        if (heldCount > 0 && sv > 1.0e-3f)
        {
            int note; float vel;
            pickNote (note, vel);
            emit (pos, note, std::min (1.0f, vel * sv), true);
            activeNote = note;
            gateRemaining = std::max (1.0, (double) cfg.gate * cfg.samplesPerStep);
        }
    }

    void pickNote (int& outNote, float& outVel)
    {
        const int n = heldCount;
        const int oct = std::max (1, std::min (4, cfg.octaves));
        const int total = std::max (1, n * oct);
        int idx = 0;
        switch (cfg.mode)
        {
            case Up:       idx = seqCursor % total; break;
            case Down:     idx = (total - 1) - (seqCursor % total); break;
            case UpDown:   { const int period = (total <= 1) ? 1 : (2 * total - 2);
                             const int p = seqCursor % period; idx = (p < total) ? p : (period - p); } break;
            case Random:   idx = (int) (nextRand() % (std::uint32_t) total); break;
            case AsPlayed: idx = seqCursor % total; break;
            default:       idx = seqCursor % total; break;
        }
        ++seqCursor;

        const int within = idx % n;
        const int octave = idx / n;
        if (cfg.mode == AsPlayed) { outNote = heldNote[(std::size_t) within]; outVel = heldVel[(std::size_t) within]; }
        else                      { outNote = sortNote[(std::size_t) within]; outVel = sortVel[(std::size_t) within]; }
        outNote = std::min (127, std::max (0, outNote + 12 * octave));
    }

    void addHeld (int note, float vel)
    {
        for (int i = 0; i < heldCount; ++i) if (heldNote[(std::size_t) i] == note) { heldVel[(std::size_t) i] = vel; rebuildSorted(); return; }
        if (heldCount >= kMaxHeld) return;
        heldNote[(std::size_t) heldCount] = note;
        heldVel[(std::size_t) heldCount]  = vel;
        ++heldCount;
        rebuildSorted();
    }
    void removeHeld (int note)
    {
        for (int i = 0; i < heldCount; ++i)
            if (heldNote[(std::size_t) i] == note)
            {
                for (int j = i; j < heldCount - 1; ++j) { heldNote[(std::size_t) j] = heldNote[(std::size_t) (j + 1)]; heldVel[(std::size_t) j] = heldVel[(std::size_t) (j + 1)]; }
                --heldCount; rebuildSorted(); return;
            }
    }
    void rebuildSorted()
    {
        for (int i = 0; i < heldCount; ++i) { sortNote[(std::size_t) i] = heldNote[(std::size_t) i]; sortVel[(std::size_t) i] = heldVel[(std::size_t) i]; }
        for (int i = 1; i < heldCount; ++i)                // insertion sort by note (small n)
            for (int j = i; j > 0 && sortNote[(std::size_t) (j - 1)] > sortNote[(std::size_t) j]; --j)
            { std::swap (sortNote[(std::size_t) (j - 1)], sortNote[(std::size_t) j]); std::swap (sortVel[(std::size_t) (j - 1)], sortVel[(std::size_t) j]); }
    }

    std::uint32_t nextRand() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

    Config cfg;
    std::array<int,   kMaxHeld> heldNote { };
    std::array<float, kMaxHeld> heldVel  { };
    std::array<int,   kMaxHeld> sortNote { };
    std::array<float, kMaxHeld> sortVel  { };
    int heldCount = 0, keysDown = 0;
    int activeNote = -1;
    int stepIndex = -1;
    double sampleInStep = 0.0, gateRemaining = 0.0;
    int seqCursor = 0;
    bool started = false;
    std::uint32_t rng = 0x9e3779b9u;
};
