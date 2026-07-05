#pragma once
#include "PolyBlepOscillator.h"
#include "SVFilter.h"
#include "ADSREnvelope.h"
#include <random>
#include <algorithm>
#include <cstdint>

// ============================================================================
// One voice = the complete mono signal chain for one held note:
//
//   OSC1 ──┐
//   OSC2 ──┼── mix ──> SVF filter ──> VCA ──> out
//   NOISE ─┘              ▲            ▲
//                    filter ADSR    amp ADSR
//                    (+ keytrack,
//                     env amount,
//                     LFO cutoff mod)
//
// Voices are dumb on purpose: they hold no parameter state of their own.
// The engine reads the APVTS once per block and pushes a plain-old-data
// `VoiceParams` struct into every voice. This keeps voices trivially
// copyable/testable and keeps all APVTS access in one place.
// ============================================================================

struct VoiceParams
{
    // osc
    int    osc1Wave = 0, osc2Wave = 0;
    float  osc1Octave = 0, osc2Octave = 0;
    float  osc1Detune = 0, osc2Detune = 0;     // cents
    float  osc1PW = 0.5f, osc2PW = 0.5f;
    float  oscMix = 0.5f, noiseLevel = 0.0f;

    // filter
    int    filterType = 0;
    float  cutoffHz = 2000.0f, resonance = 0.1f;
    float  filterEnvAmt = 0.3f;                // -1..1
    float  keytrack = 0.0f;                    // 0..1

    // envelopes
    float  ampA = 0.005f, ampD = 0.1f, ampS = 0.8f, ampR = 0.15f;
    float  fltA = 0.005f, fltD = 0.2f, fltS = 0.3f, fltR = 0.2f;

    // modulation (already computed by engine from LFO + destination routing)
    float  pitchModSemis = 0.0f;               // LFO -> pitch
    float  cutoffModOct  = 0.0f;               // LFO -> cutoff, in octaves
    float  pwMod         = 0.0f;               // LFO -> pulse width
};

class SynthVoice
{
public:
    // Set before prepare(): oscillator anti-aliasing quality.
    void setOscQuality (PolyBlepOscillator::Quality q)
    {
        osc1.setQuality (q);
        osc2.setQuality (q);
    }

    void prepare (double sampleRate)
    {
        osc1.prepare (sampleRate);
        osc2.prepare (sampleRate);
        filter.prepare (sampleRate);
        ampEnv.prepare (sampleRate);
        fltEnv.prepare (sampleRate);
    }

    void noteOn (int note, float vel, std::uint64_t stamp)
    {
        // Only clear DSP state for a genuinely fresh voice. On a retrigger or a
        // steal (voice already sounding) we keep oscillator phase and filter
        // state continuous — a phase reset there is an audible click. The amp
        // envelope retriggers from its current level, so the transition is
        // click-free (fresh voices start from level 0, so phase is irrelevant).
        const bool wasIdle = ! active;

        midiNote  = note;
        velocity  = vel;
        timestamp = stamp;

        if (wasIdle)
        {
            osc1.reset();
            osc2.reset();
            filter.reset();
        }
        ampEnv.noteOn();
        fltEnv.noteOn();
        active = true;
    }

    void noteOff()
    {
        ampEnv.noteOff();
        fltEnv.noteOff();
    }

    void steal() { ampEnv.quickRelease(); fltEnv.quickRelease(); }

    bool isActive() const  { return active; }
    int  getNote() const   { return midiNote; }
    std::uint64_t getTimestamp() const { return timestamp; }

    // Render `numSamples` and ADD into the (mono) output buffer.
    void render (float* out, int numSamples, const VoiceParams& p)
    {
        if (! active)
            return;

        applyParams (p);

        const float trackOct = p.keytrack * (midiNote - 60) / 12.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            const float ampLevel = ampEnv.nextSample();
            const float fltLevel = fltEnv.nextSample();

            if (! ampEnv.isActive())      // envelope finished -> voice done
            {
                active = false;
                break;
            }

            // --- filter cutoff modulation, all in octaves then to Hz -------
            // Control-rate: recompute the (expensive: std::tan) coefficient every
            // kCutoffInterval samples. At 48 kHz that's ~0.33 ms granularity —
            // inaudible for envelope/LFO sweeps and a big CPU saving vs per-sample.
            if ((i & (kCutoffInterval - 1)) == 0)
            {
                const float envOct = p.filterEnvAmt * fltLevel * 5.0f;       // +/-5 oct sweep
                const float fc = p.cutoffHz * std::exp2 (envOct + trackOct + p.cutoffModOct);
                filter.setCutoff (fc, p.resonance);
            }

            // --- oscillators ----------------------------------------------
            float s = osc1.nextSample() * (1.0f - p.oscMix)
                    + osc2.nextSample() * p.oscMix
                    + noise() * p.noiseLevel;

            s = filter.process (s);
            out[i] += s * ampLevel * velocity;
        }
    }

private:
    // Filter-coefficient update interval (power of two for the bit mask).
    static constexpr int kCutoffInterval = 16;

    void applyParams (const VoiceParams& p)
    {
        const double f0 = 440.0 * std::exp2 ((midiNote - 69 + p.pitchModSemis) / 12.0);

        osc1.setWave (static_cast<PolyBlepOscillator::Wave> (p.osc1Wave));
        osc2.setWave (static_cast<PolyBlepOscillator::Wave> (p.osc2Wave));
        osc1.setFrequency (f0 * std::exp2 (p.osc1Octave + p.osc1Detune / 1200.0f));
        osc2.setFrequency (f0 * std::exp2 (p.osc2Octave + p.osc2Detune / 1200.0f));
        osc1.setPulseWidth (std::clamp (p.osc1PW + p.pwMod, 0.05f, 0.95f));
        osc2.setPulseWidth (std::clamp (p.osc2PW + p.pwMod, 0.05f, 0.95f));

        filter.setType (static_cast<SVFilter::Type> (p.filterType));
        ampEnv.setParameters (p.ampA, p.ampD, p.ampS, p.ampR);
        fltEnv.setParameters (p.fltA, p.fltD, p.fltS, p.fltR);

        // TODO: glide/portamento — slew f0 toward target in mono/legato modes.
    }

    float noise()
    {
        // Fast xorshift white noise; good enough, allocation-free.
        nz ^= nz << 13; nz ^= nz >> 17; nz ^= nz << 5;
        return static_cast<float> (static_cast<std::int32_t> (nz)) / 2147483648.0f;
    }

    PolyBlepOscillator osc1, osc2;
    SVFilter           filter;
    ADSREnvelope       ampEnv, fltEnv;

    int   midiNote  = 60;
    float velocity  = 0.0f;
    bool  active    = false;
    std::uint64_t timestamp = 0;   // for oldest-note stealing
    std::uint32_t nz = 0x12345678;
};
