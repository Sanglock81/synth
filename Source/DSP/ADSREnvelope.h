#pragma once
#include <cmath>
#include <algorithm>

// ============================================================================
// ADSR envelope, hand-rolled, with one-pole exponential segments.
//
// WHY EXPONENTIAL:
// Analog envelope generators charge/discharge capacitors, producing
// exponential curves. Linear digital envelopes sound flat and "computery" —
// the attack in particular feels sluggish and the release feels abrupt.
// We model each segment as a one-pole toward an (overshooting) target,
// which is both cheap and exactly what an RC circuit does.
//
// The attack targets slightly *above* 1.0 and we clip at 1.0 — this is the
// standard trick to make exponential attacks reach the top in finite time
// (a pure exponential never actually arrives).
// ============================================================================

class ADSREnvelope
{
public:
    enum class Stage { Idle, Attack, Decay, Sustain, Release };

    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate;
        reset();
    }

    void reset()
    {
        stage = Stage::Idle;
        level = 0.0;
    }

    void setParameters (double attackS, double decayS, double sustainLvl, double releaseS)
    {
        attackCoef  = coefForTime (attackS);
        decayCoef   = coefForTime (decayS);
        releaseCoef = coefForTime (releaseS);
        sustain     = std::clamp (sustainLvl, 0.0, 1.0);
    }

    void noteOn()
    {
        // Retrigger from current level, not zero -> no click on fast retrigs.
        stage = Stage::Attack;
    }

    void noteOff()
    {
        if (stage != Stage::Idle)
            stage = Stage::Release;
    }

    // Hard steal: fast fade used by the voice allocator to avoid clicks.
    void quickRelease()
    {
        releaseCoef = coefForTime (0.005);
        stage = Stage::Release;
    }

    bool isActive() const { return stage != Stage::Idle; }

    float nextSample()
    {
        switch (stage)
        {
            case Stage::Attack:
                level += attackCoef * (attackTarget - level);
                if (level >= 1.0) { level = 1.0; stage = Stage::Decay; }
                break;

            case Stage::Decay:
                level += decayCoef * (sustain - level);
                if (level <= sustain + 1.0e-4) { level = sustain; stage = Stage::Sustain; }
                break;

            case Stage::Sustain:
                level = sustain;
                break;

            case Stage::Release:
                level += releaseCoef * (0.0 - level);
                if (level <= 1.0e-5) { level = 0.0; stage = Stage::Idle; }
                break;

            case Stage::Idle:
                level = 0.0;
                break;
        }
        return static_cast<float> (level);
    }

private:
    // One-pole coefficient that traverses ~most of the distance in `seconds`.
    double coefForTime (double seconds) const
    {
        seconds = std::max (seconds, 0.0005);
        return 1.0 - std::exp (-1.0 / (seconds * sampleRate * 0.3));
    }

    static constexpr double attackTarget = 1.3;   // overshoot -> finite attack

    double sampleRate = 44100.0;
    double level = 0.0, sustain = 0.8;
    double attackCoef = 0.0, decayCoef = 0.0, releaseCoef = 0.0;
    Stage  stage = Stage::Idle;
};
