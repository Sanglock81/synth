#pragma once
#include <cmath>
#include <random>

// ============================================================================
// LFO. Global (one instance in the engine, shared by all voices) — this
// mirrors classic polysynths where a single LFO modulates every voice in
// phase, giving that cohesive "the whole synth is wobbling" character.
// Per-voice LFOs (for e.g. free-running vibrato per note) are a v2 option.
//
// Runs at control rate: updated once per block or every N samples, not
// per-sample. LFO destinations change slowly enough that this is inaudible
// and it keeps CPU essentially free.
//
// TODO v2: MIDI clock sync (rate as note divisions) — hooks into the
// Ableton/MC8 rig for tempo-locked filter wobble.
// ============================================================================

class LFO
{
public:
    enum class Shape { Triangle, Sine, Square, SampleHold };

    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate;
        phase = 0.0;
    }

    void   setRate (double hz)   { phaseInc = hz / sampleRate; }
    void   setShape (Shape s)    { shape = s; }
    double currentPhase() const  { return phase; }        // for freezing on sync-off

    // Advance by `numSamples` and return the current bipolar value [-1, 1] (free-running).
    float advance (int numSamples)
    {
        const double oldPhase = phase;
        phase += phaseInc * numSamples;
        while (phase >= 1.0)
            phase -= 1.0;
        return valueForWrap (oldPhase);
    }

    // J1: set the phase ABSOLUTELY (for a tempo-synced, transport-position-derived LFO) and return
    // its value. S&H latches a new value when the phase wraps past a cycle boundary.
    float setPhase (double phase01)
    {
        const double oldPhase = phase;
        phase = phase01 - std::floor (phase01);
        return valueForWrap (oldPhase);
    }

private:
    float valueForWrap (double oldPhase)
    {
        switch (shape)
        {
            case Shape::Sine:     return static_cast<float> (std::sin (phase * 6.283185307179586));
            case Shape::Triangle: return static_cast<float> (4.0 * std::abs (phase - 0.5) - 1.0);
            case Shape::Square:   return phase < 0.5 ? 1.0f : -1.0f;
            case Shape::SampleHold:
                if (phase < oldPhase)                 // wrapped -> new random value
                    heldValue = dist (rng);
                return heldValue;
        }
        return 0.0f;
    }

    Shape  shape = Shape::Triangle;
    double sampleRate = 44100.0;
    double phase = 0.0, phaseInc = 0.0;

    std::minstd_rand rng { 0x5EED };
    std::uniform_real_distribution<float> dist { -1.0f, 1.0f };
    float heldValue = 0.0f;
};
