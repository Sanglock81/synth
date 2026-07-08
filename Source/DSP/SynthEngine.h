#pragma once
#include "SynthVoice.h"
#include "LFO.h"
#include <array>
#include <atomic>
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
        // Fixed voice-sum headroom trim: 1/sqrt(N), the equal-power sum rule for
        // quasi-uncorrelated sources. Applied ONCE to the mono sum (never per
        // active-voice count — dynamic scaling pumps as voices start/stop). This
        // keeps typical playing well under full-scale so the output stays clean;
        // pathological dense chords are caught by the processor's safety clipper.
        voiceTrim = 1.0f / std::sqrt ((float) activeVoiceLimit);
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
    // `part` selects which part's params the voice renders with (7C). The 16-voice
    // pool is SHARED across parts with global oldest-note stealing (per-part
    // reservation is future work). Voice identity is (note, part) so two parts can
    // hold the same note number without over-releasing.
    void noteOn (int note, float velocity, int part = 0)
    {
        if (polyMode != 0) { monoNoteOn (note, velocity, part); return; }

        // Reuse a voice already playing this (note, part) — retrigger.
        for (std::size_t i = 0; i < activeVoiceLimit; ++i)
            if (voices[i].isActive() && voices[i].getNote() == note && voices[i].getPart() == part)
                { sustained[i] = false; voices[i].noteOn (note, velocity, ++eventCounter, part); return; }

        // Otherwise find a free voice...
        for (std::size_t i = 0; i < activeVoiceLimit; ++i)
            if (! voices[i].isActive())
                { sustained[i] = false; voices[i].noteOn (note, velocity, ++eventCounter, part); return; }

        // ...or steal the oldest (global across parts). The voice keeps its
        // oscillator phase and filter state (SynthVoice::noteOn only clears them for
        // an idle voice) and the amp envelope retriggers from its current level, so
        // the steal is click-free without a separate fade.
        std::size_t oldest = 0;
        for (std::size_t i = 1; i < activeVoiceLimit; ++i)
            if (voices[i].getTimestamp() < voices[oldest].getTimestamp())
                oldest = i;

        sustained[oldest] = false;
        ++stealCounter;
        voices[oldest].noteOn (note, velocity, ++eventCounter, part);
    }

    // ---- observability accessors (const; for the processor's telemetry) ----
    int activeVoiceCount() const
    {
        int c = 0;
        for (auto& v : voices) if (v.isActive()) ++c;
        return c;
    }
    std::uint64_t stealCount() const { return stealCounter; }

    void noteOff (int note, int part = 0)
    {
        if (polyMode != 0) { monoNoteOff (note); return; }

        for (std::size_t i = 0; i < (std::size_t) maxVoices; ++i)
            if (voices[i].isActive() && voices[i].getNote() == note && voices[i].getPart() == part)
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

    // ---- parts (7C) --------------------------------------------------------
    static constexpr int maxParts = 4;   // v1 fixed cap: part 0 = LIVE, 1-3 = LOCKED

    // Publish a locked part's baked VoiceParams (message thread; lock-free double-
    // buffer, audio thread reads the current slot each render). No allocation.
    void setLockedPartParams (int part, const VoiceParams& vp)
    {
        if (part >= 1 && part < maxParts) lockedSlots[(std::size_t) part].publish (vp);
    }

    // The single per-voice params-selection SEAM. In 7C every note in a part uses
    // that part's params — the `note` argument is UNUSED here. A future "Kit part"
    // type will specialize this per note (each pad → its own baked params + sounding
    // pitch) WITHOUT touching the render loop below; that is the point of the seam.
    const VoiceParams& paramsFor (int part, int /*note*/) const
    {
        const int p = (part >= 0 && part < maxParts) ? part : 0;
        return partParams[(std::size_t) p];
    }

    // ---- rendering ---------------------------------------------------------
    // Renders MONO into `out`; the processor copies to both channels. `liveParams`
    // is part 0 (the LIVE part, smoothed here). Locked parts render from their
    // baked params. The global LFO / bend / vibrato are SHARED by all parts (v1).
    void render (float* out, int numSamples, VoiceParams liveParams,
                 float lfoRate, int lfoShape, float lfoDepth, int lfoDest)
    {
        lfo.setRate (lfoRate);
        lfo.setShape (static_cast<LFO::Shape> (lfoShape));

        // Snapshot the locked parts' baked params for this render (lock-free).
        for (int pt = 1; pt < maxParts; ++pt)
            partParams[(std::size_t) pt] = lockedSlots[(std::size_t) pt].current();

        // Prime part-0 smoothers to the first targets so notes don't sweep from
        // stale state on the very first block (osc levels are on/off-folded already).
        if (! smoothPrimed)
        {
            smCutoff = liveParams.cutoffHz; smReso = liveParams.resonance;
            smL1 = liveParams.osc1Level; smL2 = liveParams.osc2Level; smL3 = liveParams.osc3Level;
            smoothPrimed = true;
        }

        // Sub-chunks so part-0 cutoff/resonance/levels (and the LFO) update smoothly.
        int done = 0;
        while (done < numSamples)
        {
            const int chunk = std::min (kSmoothChunk, numSamples - done);

            smCutoff += smoothCoef * (liveParams.cutoffHz  - smCutoff);
            smReso   += smoothCoef * (liveParams.resonance - smReso);
            smL1     += smoothCoef * (liveParams.osc1Level - smL1);
            smL2     += smoothCoef * (liveParams.osc2Level - smL2);
            smL3     += smoothCoef * (liveParams.osc3Level - smL3);

            partParams[0] = liveParams;                     // part 0 = smoothed LIVE
            partParams[0].cutoffHz  = smCutoff;
            partParams[0].resonance = smReso;
            partParams[0].osc1Level = smL1;
            partParams[0].osc2Level = smL2;
            partParams[0].osc3Level = smL3;

            // Shared modulation this chunk (one global LFO + bend + vibrato).
            const float lfoVal = lfo.advance (chunk) * lfoDepth;
            float modPitch = 0.0f, modCutoffOct = 0.0f, modPw = 0.0f;
            switch (lfoDest)
            {
                case 1: modPitch     = lfoVal * 2.0f;  break;   // +/-2 semis
                case 2: modCutoffOct = lfoVal * 3.0f;  break;   // +/-3 oct
                case 3: modPw        = lfoVal * 0.45f; break;
                default: break;
            }
            const float vib = vibratoLFO.advance (chunk) * modWheel * kVibratoSemis;
            modPitch += pitchBendSemis + vib;

            // Each active voice renders with ITS part's params + the shared mods.
            for (auto& v : voices)
            {
                if (! v.isActive()) continue;
                VoiceParams p = paramsFor (v.getPart(), v.getNote());
                p.pitchModSemis += modPitch;                // base pitchModSemis is 0
                p.cutoffModOct   = modCutoffOct;
                p.pwMod          = modPw;
                v.render (out + done, chunk, p);
            }

            done += chunk;
        }

        // Fixed headroom trim on the summed voices (see setMaxVoices).
        for (int i = 0; i < numSamples; ++i)
            out[i] *= voiceTrim;
    }

private:
    // ---- mono / legato (last-note priority, voice 0) -----------------------
    // Mono/legato is single-timbre in v1 (voice 0). `part` is carried through so a
    // routed surface in mono still selects its params; multitimbral play uses poly.
    void monoNoteOn (int note, float velocity, int part = 0)
    {
        const bool hadNote = numHeld > 0;
        pushHeld (note);
        sustained[0] = false;

        // Legato: while a note is already sounding, glide to the new pitch
        // without retriggering the envelope. Mono (and the first note): retrigger.
        if (polyMode == 2 && hadNote && voices[0].isActive())
            voices[0].changeNote (note, ++eventCounter);
        else
            voices[0].noteOn (note, velocity, ++eventCounter, part);
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
    float voiceTrim = 0.25f;                        // 1/sqrt(maxVoices); headroom trim

    // Parts (7C): part 0 = LIVE (smoothed per chunk), parts 1-3 = LOCKED (baked,
    // published lock-free via a double buffer). partParams[] is the audio-thread
    // working copy that paramsFor() returns.
    struct LockedSlot
    {
        VoiceParams buf[2];
        std::atomic<int> idx { 0 };
        const VoiceParams& current() const { return buf[(std::size_t) idx.load (std::memory_order_acquire)]; }
        void publish (const VoiceParams& vp)          // message thread
        {
            const int w = 1 - idx.load (std::memory_order_relaxed);
            buf[(std::size_t) w] = vp;
            idx.store (w, std::memory_order_release);
        }
    };
    std::array<VoiceParams, maxParts> partParams {};
    std::array<LockedSlot,  maxParts> lockedSlots {};
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
