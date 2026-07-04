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

    void prepare (double sampleRate)
    {
        for (auto& v : voices)
            v.prepare (sampleRate);
        lfo.prepare (sampleRate);
        eventCounter = 0;
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

        // ...or steal the oldest.
        auto* oldest = &voices[0];
        for (auto& v : voices)
            if (v.getTimestamp() < oldest->getTimestamp())
                oldest = &v;

        oldest->steal();
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
        // Control-rate LFO: one value per block. Fine for v1 block sizes
        // (128-256 samples ~= 3-6 ms). TODO: sub-block chunks of 32 for
        // smoother fast LFOs.
        lfo.setRate (lfoRate);
        lfo.setShape (static_cast<LFO::Shape> (lfoShape));
        const float lfoVal = lfo.advance (numSamples) * lfoDepth;

        switch (lfoDest)
        {
            case 1: params.pitchModSemis = lfoVal * 2.0f;  break;   // +/-2 semis
            case 2: params.cutoffModOct  = lfoVal * 3.0f;  break;   // +/-3 oct
            case 3: params.pwMod         = lfoVal * 0.45f; break;
            default: break;                                          // off
        }

        for (auto& v : voices)
            v.render (out, numSamples, params);
    }

private:
    std::array<SynthVoice, maxVoices> voices;
    LFO lfo;
    std::uint64_t eventCounter = 0;
};
