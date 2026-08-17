// Synth — Copyright © 2026 John L Farmer. Licensed under AGPLv3; see LICENSE.
#pragma once
#include <array>
#include <algorithm>

// ============================================================================
// Multi-row step sequencer (drum grid) — JUCE-free, RT-safe (fixed storage).
//
// 8 rows x 16 steps (16th notes of one measure). Each row triggers one note (a kit
// pad by default) and has a mute; each step is off / on / ACCENT (higher velocity).
// On every 16th it fires the enabled rows' notes at the target part, holding each for
// GATE of a step. Clocked at the same 16th rate + SWING as the arpeggiator (shared
// tempo). Rows sound independently, so a dense pattern layers cleanly.
//
// The owner (the processor) sets the pattern (message thread) and calls process()
// once per audio block, feeding the emitted note events to the engine.
// ============================================================================

class StepSequencer
{
public:
    static constexpr int kRows  = 8;
    static constexpr int kSteps = 16;
    enum Cell : unsigned char { Off = 0, On = 1 };   // (accent absorbed into per-step velocity)

    // The eight rows a drummer reaches for first: kick, snare, rim, closed hat, open hat,
    // crash, ride, low tom. Trigger notes are THIS PROJECT'S kit convention, not GM — every
    // factory kit puts kick on 36, snare on 38, rim on 39, closed hat on 42 and open hat on
    // 43, and the tom trio on 44/45/46 (GM would say 46 is the open hat, which is wrong here).
    // Ride 47 / crash 48 follow the 909/707/RX5/R50 cluster, the one pair the library already
    // used consistently; the remaining kits are brought onto it by the kit harmonization pass.
    //
    // ONE definition, used by the DSP default, the processor's default, and the fallback for a
    // state tree that predates the seq_notes property — those had drifted apart, and the
    // fallback (a chromatic 36..43 run) was silently winning at startup.
    static std::array<int, kRows> defaultNotes()
    { return { { 36, 38, 39, 42, 43, 48, 47, 44 } }; }

    struct Config
    {
        bool   enabled = false;
        double samplesPerStep = 6000;   // 16th-note length (from tempo)
        float  gate  = 0.5f;            // 0..1 of a step
        float  swing = 0.0f;            // 0..~0.7 (delays odd 16ths)
        std::array<std::array<unsigned char, kSteps>, kRows> cells { };
        // Per-step velocity PERCENT (task #54): 0 uses the default (100%); 10..200 sets it
        // explicitly (accent = >100). Emitted velocity = clamp(vel% / 100, 0..1).
        std::array<std::array<unsigned char, kSteps>, kRows> vel { };
        std::array<int, kRows>  note = defaultNotes();
        std::array<bool, kRows> mute { };
    };

    void setConfig (const Config& c) { cfg = c; }
    bool enabled() const { return cfg.enabled; }
    int  currentStep() const { return stepIndex; }        // -1 idle; UI playhead

    void reset()
    {
        for (auto& a : activeNote) a = -1;
        for (auto& g : gateRemaining) g = 0.0;
        stepIndex = -1; sampleInStep = 0.0; started = false;
    }

    // Release any held notes (note-offs), leaving the clock running. Call this when the
    // TARGET part changes so the old part's gated note doesn't hang (its note-off would
    // otherwise be dispatched to the new target). emit(sampleOffset, note, 0, false).
    template <typename Emit>
    void releaseActive (Emit&& emit)
    {
        for (int r = 0; r < kRows; ++r) if (activeNote[(std::size_t) r] >= 0) { emit (0, activeNote[(std::size_t) r], 0.0f, false); activeNote[(std::size_t) r] = -1; }
    }

    // Release held notes AND reset the clock. The owner MUST call this when the sequencer
    // is disabled while the render path won't call process() again, else the last gate hangs.
    template <typename Emit>
    void flush (Emit&& emit)
    {
        releaseActive (emit);
        stepIndex = -1; sampleInStep = 0.0; started = false;
    }

    // Re-anchor to the shared transport downbeat (task #53): the next process() restarts
    // from step 0 at offset 0. Swing self-accumulates WITHIN a bar; the owner calls this at
    // each bar boundary so bar-1 always snaps to the transport (bounded per-bar float drift)
    // and the sequencer's step-1 coincides with the arp downbeat + looper boundary. The DSP
    // process()/swing/gate logic is untouched — this is a phase reset the owner drives.
    void realign() { started = false; }

    // #145: PHASE-ONLY bar-boundary re-anchor. The owner calls this every bar with the transport's
    // grid position (step + fractional samples into it). It bounds the per-bar float/swing drift
    // WITHOUT emitting — unlike realign()'s force-fire of step 0, which double-triggered the
    // downbeat whenever a bar boundary fell mid-block (the natural clock had already fired step 0 in
    // the prior block), a heavier hit recurring with a block-size-dependent period. We only correct
    // the fractional phase when the seq and transport agree on the current step; if they are within
    // a step of the boundary we leave the clock so the natural crossing fires it exactly once.
    void realignPhase (int transportStep, double transportInto)
    {
        if (! started) return;
        const int g = ((transportStep % kSteps) + kSteps) % kSteps;
        if (stepIndex == g)                                        // already fired step g: snap the phase only
            sampleInStep = std::clamp (transportInto, 0.0, cfg.samplesPerStep);
        else                                                       // step g not fired yet (due, or off after a
        {                                                          // tempo change): re-anchor to fire it ONCE now
            stepIndex = (g - 1 + kSteps) % kSteps;
            sampleInStep = cfg.samplesPerStep * ((stepIndex % 2 == 0) ? (1.0 + cfg.swing) : (1.0 - cfg.swing));
        }
    }

    // Start the pattern ALREADY LOCKED to the shared grid, instead of firing step 0 at the
    // instant of enabling. `g` = the bar's current 16th-step index (looper.position()/step),
    // `intoStep` = samples elapsed into it. Enabling mid-bar therefore picks up at the beat the
    // transport is actually on — step 0 still lands on the bar downbeat — so nothing plays a
    // partial bar off-grid and there's no visible snap-to-0 when the clock wraps. The owner calls
    // this on the enable edge (task #127). A hit that lands exactly on a step boundary still fires.
    void startAtGrid (int g, double intoStep)
    {
        started = true;
        g = ((g % kSteps) + kSteps) % kSteps;
        const double stepLen = cfg.samplesPerStep;                 // un-swung anchor; swing accrues after
        if (intoStep < 1.0)                                        // on the boundary -> fire this step now
        {
            stepIndex = (g - 1 + kSteps) % kSteps;
            sampleInStep = stepLen;                                // process() immediately doStep()s -> g
        }
        else                                                      // mid-step -> pick up here, next boundary fires g+1
        {
            stepIndex = g;
            sampleInStep = std::min (intoStep, stepLen);
        }
    }

    // Emit this block's note events. emit(sampleOffset, note, velocity, isOn).
    template <typename Emit>
    void process (int numSamples, Emit&& emit)
    {
        if (! cfg.enabled) { flush (emit); return; }

        double pos = 0.0;
        if (! started) { started = true; stepIndex = -1; sampleInStep = 0.0; doStep (0, emit); }

        const double eps = 1.0e-6;
        while (pos < numSamples)
        {
            const double stepLen = cfg.samplesPerStep * ((stepIndex % 2 == 0) ? (1.0 + cfg.swing) : (1.0 - cfg.swing));
            double adv = std::min ((double) numSamples - pos, stepLen - sampleInStep);
            for (int r = 0; r < kRows; ++r) if (activeNote[(std::size_t) r] >= 0) adv = std::min (adv, gateRemaining[(std::size_t) r]);

            pos += adv; sampleInStep += adv;
            bool acted = false;
            for (int r = 0; r < kRows; ++r)
                if (activeNote[(std::size_t) r] >= 0)
                {
                    gateRemaining[(std::size_t) r] -= adv;
                    if (gateRemaining[(std::size_t) r] <= eps) { emit ((int) pos, activeNote[(std::size_t) r], 0.0f, false); activeNote[(std::size_t) r] = -1; acted = true; }
                }
            if (sampleInStep >= stepLen - eps) { sampleInStep -= stepLen; doStep ((int) pos, emit); acted = true; }
            if (! acted) break;
        }
    }

private:
    template <typename Emit>
    void doStep (int pos, Emit&& emit)
    {
        stepIndex = (stepIndex + 1) % kSteps;
        for (int r = 0; r < kRows; ++r)
        {
            if (cfg.mute[(std::size_t) r]) continue;
            const unsigned char c = cfg.cells[(std::size_t) r][(std::size_t) stepIndex];
            if (c == Off) continue;
            if (activeNote[(std::size_t) r] >= 0) { emit (pos, activeNote[(std::size_t) r], 0.0f, false); activeNote[(std::size_t) r] = -1; }
            const int note = std::min (127, std::max (0, cfg.note[(std::size_t) r]));
            const unsigned char vp = cfg.vel[(std::size_t) r][(std::size_t) stepIndex];   // per-step velocity %
            // 10..200 % -> a 0.1..2.0 velocity SCALAR (0 => default 100 % = 1.0). NOT clamped
            // to 1.0: 100 % keeps the old behaviour, > 100 % accents (the voice's vel->amp and
            // vel->cutoff push louder AND brighter), < 100 % ghosts. The output safety clipper
            // guards the bus, so an over-unity accent can't breach +/-1.0.
            const float velF = (vp == 0 ? 100 : (int) vp) / 100.0f;
            emit (pos, note, velF, true);
            activeNote[(std::size_t) r] = note;
            gateRemaining[(std::size_t) r] = std::max (1.0, (double) cfg.gate * cfg.samplesPerStep);
        }
    }

    Config cfg;
    std::array<int, kRows>    activeNote { { -1, -1, -1, -1, -1, -1, -1, -1 } };
    std::array<double, kRows> gateRemaining { };
    int    stepIndex = -1;
    double sampleInStep = 0.0;
    bool   started = false;
};
