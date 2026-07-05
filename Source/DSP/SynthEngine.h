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
        vibratoLFO.prepare (sampleRate);
        vibratoLFO.setShape (LFO::Shape::Sine);
        vibratoLFO.setRate (5.5);                    // classic vibrato rate
        eventCounter = 0;

        // One-pole smoothing coefficient (~8 ms) applied per kSmoothChunk samples,
        // to kill zipper on cutoff / resonance / osc-mix under knob/automation steps.
        smoothCoef = 1.0f - std::exp (-float (kSmoothChunk) / (0.008f * float (sampleRate)));
        smoothPrimed = false;
    }

    // Max simultaneously-sounding voices (voice pool is always maxVoices; this
    // caps how many are allocated). The live ThinkPad profile caps this to keep
    // worst-case CPU under budget; studio can use the full pool.
    void setMaxVoices (int n)
    {
        activeVoiceLimit = (std::size_t) std::clamp (n, 1, maxVoices);
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

    // Poly (0) / Mono (1) / Legato (2). Switching mode releases everything so no
    // note gets stranded in the note stack.
    void setPolyMode (int mode)
    {
        if (mode != polyMode)
        {
            numHeld = 0;
            for (std::size_t i = 0; i < (std::size_t) maxVoices; ++i) { voices[i].noteOff(); sustained[i] = false; }
        }
        polyMode = mode;
    }

    // ---- MIDI (called from processBlock with sample-accurate offsets) -----
    void noteOn (int note, float velocity)
    {
        if (polyMode != 0) { monoNoteOn (note, velocity); return; }

        // Reuse a voice already playing this note (retrigger) if present.
        for (std::size_t i = 0; i < activeVoiceLimit; ++i)
            if (voices[i].isActive() && voices[i].getNote() == note)
                { sustained[i] = false; voices[i].noteOn (note, velocity, ++eventCounter); return; }

        // Otherwise find a free voice...
        for (std::size_t i = 0; i < activeVoiceLimit; ++i)
            if (! voices[i].isActive())
                { sustained[i] = false; voices[i].noteOn (note, velocity, ++eventCounter); return; }

        // ...or steal the oldest. The voice keeps its oscillator phase and
        // filter state (SynthVoice::noteOn only clears them for an idle voice)
        // and the amp envelope retriggers from its current level, so the steal
        // is click-free without a separate fade.
        std::size_t oldest = 0;
        for (std::size_t i = 1; i < activeVoiceLimit; ++i)
            if (voices[i].getTimestamp() < voices[oldest].getTimestamp())
                oldest = i;

        sustained[oldest] = false;
        ++stealCounter;
        voices[oldest].noteOn (note, velocity, ++eventCounter);
    }

    // ---- observability accessors (const; for the processor's telemetry) ----
    int activeVoiceCount() const
    {
        int c = 0;
        for (auto& v : voices) if (v.isActive()) ++c;
        return c;
    }
    std::uint64_t stealCount() const { return stealCounter; }

    void noteOff (int note)
    {
        if (polyMode != 0) { monoNoteOff (note); return; }

        for (std::size_t i = 0; i < (std::size_t) maxVoices; ++i)
            if (voices[i].isActive() && voices[i].getNote() == note)
            {
                if (sustainPedal) sustained[i] = true;   // held by damper
                else              voices[i].noteOff();
            }
    }

    void allNotesOff()
    {
        for (std::size_t i = 0; i < (std::size_t) maxVoices; ++i) { voices[i].noteOff(); sustained[i] = false; }
        sustainPedal = false;
        numHeld = 0;
    }

    // ---- performance controllers (from any device) -------------------------
    void setPitchBend (float semitones) { pitchBendSemis = semitones; }
    void setModWheel   (float amount01) { modWheel = amount01; }        // -> vibrato depth

    // Sustain pedal (CC64). While down, note-offs are deferred; on release the
    // held notes are let go. The Korg B2's damper is the primary expression.
    void setSustainPedal (bool on)
    {
        if (sustainPedal && ! on)                        // pedal released
            for (std::size_t i = 0; i < (std::size_t) maxVoices; ++i)
                if (sustained[i]) { voices[i].noteOff(); sustained[i] = false; }
        sustainPedal = on;
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
        // state on the very first block. The osc levels the processor passes are
        // already the on/off-folded effective levels (off -> 0), so smoothing
        // them gives click-free kill-switch toggles.
        if (! smoothPrimed)
        {
            smCutoff = params.cutoffHz; smReso = params.resonance;
            smL1 = params.osc1Level; smL2 = params.osc2Level; smL3 = params.osc3Level;
            smoothPrimed = true;
        }

        // Render in sub-chunks so cutoff/resonance/osc levels (and the LFO) update
        // smoothly across the block instead of stepping once per block.
        int done = 0;
        while (done < numSamples)
        {
            const int chunk = std::min (kSmoothChunk, numSamples - done);

            smCutoff += smoothCoef * (params.cutoffHz  - smCutoff);
            smReso   += smoothCoef * (params.resonance - smReso);
            smL1     += smoothCoef * (params.osc1Level - smL1);
            smL2     += smoothCoef * (params.osc2Level - smL2);
            smL3     += smoothCoef * (params.osc3Level - smL3);

            VoiceParams p = params;
            p.cutoffHz  = smCutoff;
            p.resonance = smReso;
            p.osc1Level = smL1;
            p.osc2Level = smL2;
            p.osc3Level = smL3;

            const float lfoVal = lfo.advance (chunk) * lfoDepth;
            switch (lfoDest)
            {
                case 1: p.pitchModSemis = lfoVal * 2.0f;  break;   // +/-2 semis
                case 2: p.cutoffModOct  = lfoVal * 3.0f;  break;   // +/-3 oct
                case 3: p.pwMod         = lfoVal * 0.45f; break;
                default: break;                                    // off
            }

            // Performance pitch: bend (+/-2 semis) + mod-wheel vibrato, on top of
            // any LFO->pitch routing above.
            const float vib = vibratoLFO.advance (chunk) * modWheel * kVibratoSemis;
            p.pitchModSemis += pitchBendSemis + vib;

            for (auto& v : voices)
                v.render (out + done, chunk, p);

            done += chunk;
        }
    }

private:
    // ---- mono / legato (last-note priority, voice 0) -----------------------
    void monoNoteOn (int note, float velocity)
    {
        const bool hadNote = numHeld > 0;
        pushHeld (note);
        sustained[0] = false;

        // Legato: while a note is already sounding, glide to the new pitch
        // without retriggering the envelope. Mono (and the first note): retrigger.
        if (polyMode == 2 && hadNote && voices[0].isActive())
            voices[0].changeNote (note, ++eventCounter);
        else
            voices[0].noteOn (note, velocity, ++eventCounter);
    }

    void monoNoteOff (int note)
    {
        removeHeld (note);
        if (numHeld > 0)
            voices[0].changeNote (heldNotes[(std::size_t) (numHeld - 1)], ++eventCounter);  // fall back
        else if (sustainPedal)
            sustained[0] = true;
        else
            voices[0].noteOff();
    }

    void pushHeld (int note)
    {
        removeHeld (note);                          // move to top if already held
        if (numHeld < 128) heldNotes[(std::size_t) numHeld++] = note;
    }
    void removeHeld (int note)
    {
        for (int i = 0; i < numHeld; ++i)
            if (heldNotes[(std::size_t) i] == note)
            {
                for (int j = i; j < numHeld - 1; ++j) heldNotes[(std::size_t) j] = heldNotes[(std::size_t)(j + 1)];
                --numHeld;
                return;
            }
    }

    static constexpr int kSmoothChunk = 16;   // sub-block size for param smoothing

    static constexpr float kVibratoSemis = 0.5f;   // max mod-wheel vibrato depth (+/-)

    std::array<SynthVoice, maxVoices> voices;
    std::array<bool, maxVoices> sustained {};      // key released but held by pedal
    std::size_t activeVoiceLimit = maxVoices;      // <= maxVoices; see setMaxVoices
    LFO lfo, vibratoLFO;
    std::uint64_t eventCounter = 0;
    std::uint64_t stealCounter = 0;
    double sampleRate = 0.0;
    PolyBlepOscillator::Quality oscQuality = PolyBlepOscillator::Quality::Efficient;

    // Performance controllers.
    float pitchBendSemis = 0.0f, modWheel = 0.0f;
    bool  sustainPedal = false;

    // Mono/legato state.
    int   polyMode = 0;                        // 0 poly, 1 mono, 2 legato
    std::array<int, 128> heldNotes {};         // held-note stack (last = priority)
    int   numHeld = 0;

    // Zipper smoothing state (global params).
    float smoothCoef = 0.05f, smCutoff = 0.0f, smReso = 0.0f;
    float smL1 = 0.8f, smL2 = 0.8f, smL3 = 0.0f;   // smoothed effective osc levels
    bool  smoothPrimed = false;
};
