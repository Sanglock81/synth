#pragma once
#include "SynthVoice.h"
#include "LFO.h"
#include <array>
#include <cstdint>

// ============================================================================
// The polyphonic engine. Owns the voice pool and the global LFO.
//
// Lives entirely on the audio thread. Everything here must be
// allocation-free and lock-free: fixed arrays, POD params, no std::vector
// growth, no logging.
//
// Voice allocation policy (v1): oldest-note stealing with quick-release.
// When all voices are busy, the voice that started longest ago gets a 5 ms
// fade (SynthVoice::steal) rather than a hard cut — the difference between
// an audible click and a seamless steal.
//
// TODO v2: mono/legato modes with last-note priority + glide,
//          unison (stack N voices per note with spread detune),
//          proper mod matrix replacing the single LFO destination.
// ============================================================================

class SynthEngine
{
public:
    static constexpr int maxVoices = 16;

    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate;
        for (auto& v : voices)
        {
            v.setOscQuality (oscQuality);
            v.prepare (sampleRate);
        }
        lfo.prepare (sampleRate);
        eventCounter = 0;

        // One-pole smoothing coefficient (~8 ms) applied per kSmoothChunk samples,
        // to kill zipper on cutoff / resonance / osc-mix under knob/automation steps.
        smoothCoef = 1.0f - std::exp (-float (kSmoothChunk) / (0.008f * float (sampleRate)));
        smoothPrimed = false;
    }

    // Oscillator anti-aliasing quality. Re-prepares the voices if already
    // prepared. Efficient (default) for the live ThinkPad; HQ for studio use.
    void setOscQuality (PolyBlepOscillator::Quality q)
    {
        oscQuality = q;
        if (sampleRate > 0.0)
            for (auto& v : voices)
            {
                v.setOscQuality (q);
                v.prepare (sampleRate);
            }
    }

    // ---- MIDI (called from processBlock with sample-accurate offsets) -----
    void noteOn (int note, float velocity)
    {
        // Reuse a voice already playing this note (retrigger) if present.
        for (auto& v : voices)
            if (v.isActive() && v.getNote() == note)
                { v.noteOn (note, velocity, ++eventCounter); return; }

        // Otherwise find a free voice...
        for (auto& v : voices)
            if (! v.isActive())
                { v.noteOn (note, velocity, ++eventCounter); return; }

        // ...or steal the oldest. The voice keeps its oscillator phase and
        // filter state (SynthVoice::noteOn only clears them for an idle voice)
        // and the amp envelope retriggers from its current level, so the steal
        // is click-free without a separate fade.
        auto* oldest = &voices[0];
        for (auto& v : voices)
            if (v.getTimestamp() < oldest->getTimestamp())
                oldest = &v;

        oldest->noteOn (note, velocity, ++eventCounter);
    }

    void noteOff (int note)
    {
        for (auto& v : voices)
            if (v.isActive() && v.getNote() == note)
                v.noteOff();
    }

    void allNotesOff()
    {
        for (auto& v : voices)
            v.noteOff();
    }

    // ---- rendering ---------------------------------------------------------
    // Renders MONO into `out`; the processor copies to both channels.
    // `params` is fully populated by the processor from the APVTS each block.
    void render (float* out, int numSamples, VoiceParams params,
                 float lfoRate, int lfoShape, float lfoDepth, int lfoDest)
    {
        lfo.setRate (lfoRate);
        lfo.setShape (static_cast<LFO::Shape> (lfoShape));

        // Prime smoothers to the first targets so notes don't sweep from stale
        // state on the very first block.
        if (! smoothPrimed)
        {
            smCutoff = params.cutoffHz; smReso = params.resonance; smMix = params.oscMix;
            smoothPrimed = true;
        }

        // Render in sub-chunks so cutoff/resonance/osc-mix (and the LFO) update
        // smoothly across the block instead of stepping once per block.
        int done = 0;
        while (done < numSamples)
        {
            const int chunk = std::min (kSmoothChunk, numSamples - done);

            smCutoff += smoothCoef * (params.cutoffHz  - smCutoff);
            smReso   += smoothCoef * (params.resonance - smReso);
            smMix    += smoothCoef * (params.oscMix    - smMix);

            VoiceParams p = params;
            p.cutoffHz  = smCutoff;
            p.resonance = smReso;
            p.oscMix    = smMix;

            const float lfoVal = lfo.advance (chunk) * lfoDepth;
            switch (lfoDest)
            {
                case 1: p.pitchModSemis = lfoVal * 2.0f;  break;   // +/-2 semis
                case 2: p.cutoffModOct  = lfoVal * 3.0f;  break;   // +/-3 oct
                case 3: p.pwMod         = lfoVal * 0.45f; break;
                default: break;                                    // off
            }

            for (auto& v : voices)
                v.render (out + done, chunk, p);

            done += chunk;
        }
    }

private:
    static constexpr int kSmoothChunk = 16;   // sub-block size for param smoothing

    std::array<SynthVoice, maxVoices> voices;
    LFO lfo;
    std::uint64_t eventCounter = 0;
    double sampleRate = 0.0;
    PolyBlepOscillator::Quality oscQuality = PolyBlepOscillator::Quality::Efficient;

    // Zipper smoothing state (global params).
    float smoothCoef = 0.05f, smCutoff = 0.0f, smReso = 0.0f, smMix = 0.0f;
    bool  smoothPrimed = false;
};
