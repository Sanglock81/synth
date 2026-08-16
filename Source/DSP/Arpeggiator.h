// Synth — Copyright © 2026 John L Farmer. Licensed under AGPLv3; see LICENSE.
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
        // Per-step ABSOLUTE velocity (#136): 0 = rest (step off), else the step's velocity
        // (0.1..2.0 = 10..200 %, where 100 % = full MIDI). When the ARP is active this takes
        // PRECEDENCE over how hard the note was played — the step value IS the output velocity,
        // so a pattern sounds identical however you touch the keys. > 100 % accents via the
        // voice's velocity > 1.0 boost. The velocity belongs to the STEP, not the note.
        std::array<float, kNumSteps> steps { };
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

    // Re-lock the step PHASE to the shared grid (task #53) WITHOUT resetting the pattern index.
    // The owner calls this at each bar boundary: it forces the NEXT step to fire on the downbeat
    // (setting sampleInStep to a full step so process() emits at offset 0), so the arp's steps stay
    // on the beat grid with bounded drift. But it advances the pattern normally (stepIndex+1) — it
    // does NOT snap back to step 0 at the measure. So striking a key mid-bar starts the arp there and
    // it free-runs its pattern locked to the grid, instead of skipping to the start each bar.
    void realign() { sampleInStep = stepLength(); }

    // #145 (guard 3): PHASE-ONLY bar-boundary re-anchor, same latent double-fire class as the step
    // sequencer. realign() forces the next step to fire on the downbeat (sampleInStep = a full
    // step), which advances the pattern an EXTRA step whenever the natural clock already crossed the
    // boundary in the prior block. This corrects only the fractional phase when the arp and transport
    // agree on the current step — no forced (extra) step.
    void realignPhase (int transportStep, double transportInto)
    {
        if (! started) return;
        const int g = ((transportStep % kNumSteps) + kNumSteps) % kNumSteps;
        // The arp's stepIndex tracks the transport grid (it is started via startAtGrid). If it has
        // ALREADY advanced onto grid step g (a mid-block boundary: the natural clock fired it in the
        // prior block), only snap the phase — do NOT force, or the pattern double-advances (the old
        // realign's latent bug). Otherwise grid step g is due: fire the next pattern step once, on the
        // downbeat (stepIndex is untouched, so the pattern advances normally — no reset).
        if (stepIndex == g)
            sampleInStep = std::clamp (transportInto, 0.0, stepLength());
        else
            sampleInStep = stepLength();
    }

    // Start the arp ALREADY LOCKED to the shared grid instead of firing step 0 the instant it's
    // enabled: `g` = the bar's current 16th index, `intoStep` = samples into it. So turning the arp
    // on mid-bar picks up on the beat the transport is at, matching the sequencer (task #127). A
    // strike lands within block tolerance; a boundary hit still fires this step.
    void startAtGrid (int g, double intoStep)
    {
        started = true;
        g = ((g % kNumSteps) + kNumSteps) % kNumSteps;
        const double stepLen = cfg.samplesPerStep;
        if (intoStep < 1.0)
        {
            stepIndex = (g - 1 + kNumSteps) % kNumSteps;
            sampleInStep = stepLen;
        }
        else
        {
            stepIndex = g;
            sampleInStep = std::min (intoStep, stepLen);
        }
    }

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
            int note; float playedVel;
            pickNote (note, playedVel);
            (void) playedVel;   // intentionally unused: when the ARP is active its OWN velocity wins
            // sv = this step's ABSOLUTE velocity (0.1..2.0 = 10..200 %). The ARP's per-step velocity
            // takes PRECEDENCE over how hard the note was played (#136): the step value IS the output
            // velocity (100 % = full), independent of playedVel — so an arp pattern sounds the same
            // however you touch the keys. > 100 % accents via the voice's vel->amp / vel->cutoff
            // (velocity > 1.0 is a gentle linear boost); the output safety clipper guards the bus.
            emit (pos, note, sv, true);
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
