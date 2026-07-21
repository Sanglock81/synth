#pragma once
#include "PolyBlepOscillator.h"
#include "SVFilter.h"
#include "ADSREnvelope.h"
#include "ModMatrix.h"
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
    int    osc1Wave = 0, osc2Wave = 0, osc3Wave = 0;
    float  osc1Octave = 0, osc2Octave = 0, osc3Octave = 0;
    float  osc1Detune = 0, osc2Detune = 0, osc3Detune = 0;   // cents
    float  osc1PW = 0.5f, osc2PW = 0.5f, osc3PW = 0.5f;
    // Musicality Tier 1a: per-oscillator start-phase policy (0 RESET / 1 RANDOM / 2 FREE).
    // Default 0 (RESET) keeps every note's waveform alignment bit-identical (goldens hold).
    int    osc1Phase = 0, osc2Phase = 0, osc3Phase = 0;
    // Tier 1b: analog drift amount (0..1) — one per part. 0 = bit-exact (no drift). Default 0.
    float  analog = 0.0f;

    // mixer: independent per-source levels (engine writes SMOOTHED effective
    // levels here — already folded in the on/off kill switch, so a level of ~0
    // means "skip this oscillator"). oscMix stays as a legacy/no-op field.
    float  oscMix = 0.5f, noiseLevel = 0.0f;
    float  osc1Level = 0.8f, osc2Level = 0.8f, osc3Level = 0.0f;

    // velocity routing
    float  velToAmp    = 0.7f;                  // amp = (1-v2a) + v2a*velocity
    float  velToCutoff = 0.0f;                  // adds up to +3 oct at vel=1

    // filter
    int    filterType = 0;
    float  cutoffHz = 2000.0f, resonance = 0.1f;
    float  filterEnvAmt = 0.3f;                // -1..1
    float  keytrack = 0.0f;                    // 0..1
    float  drive    = 0.0f;                    // Tier 2: in-loop filter saturation (0 = clean/bit-exact)

    // envelopes
    float  ampA = 0.005f, ampD = 0.1f, ampS = 0.8f, ampR = 0.15f;
    float  fltA = 0.005f, fltD = 0.2f, fltS = 0.3f, fltR = 0.2f;
    float  fltEnvToPitch = 0.0f;               // filter/mod env -> pitch, semitones (-48..+48)

    // modulation (already computed by engine from LFO + destination routing)
    float  pitchModSemis = 0.0f;               // LFO -> pitch
    float  cutoffModOct  = 0.0f;               // LFO -> cutoff, in octaves
    float  pwMod         = 0.0f;               // LFO -> pulse width

    // performance
    float  glideTime     = 0.0f;               // portamento seconds (0 = off)
    int    polyMode      = 0;                  // 0 poly / 1 mono / 2 legato — PER PART (carried
                                               // here so a locked part bakes its own mode; the
                                               // ENGINE reads it for note allocation, not the voice)

    // per-voice output trim (Kit parts fold each pad's level here; 1.0 = unity, so a
    // non-kit voice is bit-identical). Applied at the VCA in render().
    float  gain          = 1.0f;
};

class SynthVoice
{
public:
    // Set before prepare(): oscillator anti-aliasing quality.
    void setOscQuality (PolyBlepOscillator::Quality q)
    {
        osc1.setQuality (q);
        osc2.setQuality (q);
        osc3.setQuality (q);
    }

    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate;
        osc1.prepare (newSampleRate);
        osc2.prepare (newSampleRate);
        osc3.prepare (newSampleRate);
        filter.prepare (newSampleRate);
        ampEnv.prepare (newSampleRate);
        fltEnv.prepare (newSampleRate);
    }

    void noteOn (int note, float vel, std::uint64_t stamp, int partIndex = 0, int slot = 0, bool gen = false,
                 int pm1 = 0, int pm2 = 0, int pm3 = 0)   // Tier 1a: per-osc start-phase policy (0 RESET default)
    {
        generator = gen;      // seq/arp/looper voices yield to live-played notes when stealing
        // Only clear DSP state for a genuinely fresh voice. On a retrigger or a
        // steal (voice already sounding) we keep oscillator phase and filter
        // state continuous — a phase reset there is an audible click. The amp
        // envelope retriggers from its current level, so the transition is
        // click-free (fresh voices start from level 0, so phase is irrelevant).
        const bool wasIdle = ! active;

        midiNote  = note;
        velocity  = vel;
        timestamp = stamp;
        part      = partIndex;      // which part's params this voice renders with (7C)
        soundSlot = slot;           // which pad within a Kit part (0 for non-kit)

        if (wasIdle)
        {
            osc1.reset (startPhaseFor (pm1));   // RESET(0) is bit-exact; RANDOM draws; FREE keeps phase
            osc2.reset (startPhaseFor (pm2));
            osc3.reset (startPhaseFor (pm3));
            filter.reset();
            drift1 = drift2 = drift3 = driftPw = 0.0f;   // Tier 1b: a fresh voice starts un-drifted
            freshNote = true;              // Tier 2C: (re)decide filter oversampling at the first render
            glideNote = (float) note;      // fresh voice: no glide into the first note
        }
        ampEnv.noteOn();
        fltEnv.noteOn();
        rndState ^= rndState << 13; rndState ^= rndState >> 17; rndState ^= rndState << 5;   // fresh S&H per note
        voiceRandom = (float) (std::int32_t) rndState / 2147483648.0f;
        active = true;
    }

    // Legato: retarget pitch (glides there) without retriggering the envelope or
    // resetting phase — for mono/legato note changes while a note is held.
    void changeNote (int note, std::uint64_t stamp)
    {
        midiNote  = note;
        timestamp = stamp;
    }

    void noteOff()
    {
        ampEnv.noteOff();
        fltEnv.noteOff();
    }

    void steal() { ampEnv.quickRelease(); fltEnv.quickRelease(); }

    bool isActive() const  { return active; }
    int  getNote() const   { return midiNote; }
    int  getPart() const   { return part; }
    int  getSoundSlot() const { return soundSlot; }
    bool isGenerator() const { return generator; }
    std::uint64_t getTimestamp() const { return timestamp; }

    // Render `numSamples` and ADD into the (mono) output buffer. `mtx`/`partSrc` are the
    // optional mod matrix (#56) + its per-part source snapshot; when null/inert the render
    // is bit-identical to the pre-matrix path.
    void render (float* out, int numSamples, const VoiceParams& p,
                 const ModMatrix* mtx = nullptr, const ModSources* partSrc = nullptr)
    {
        if (! active)
            return;

        // Tier 2C: latch this note's filter oversampling at its first render. A driven or self-
        // oscillating voice runs its filter at 2x for the whole note (removes the tanh's aliasing);
        // a clean voice stays at base rate (bit-exact, pays nothing). Latching at the note boundary
        // means the rate domain never switches mid-note — no discontinuity to click. 0.98 mirrors
        // SVFilter's self-osc resonance threshold.
        if (freshNote)
        {
            filter.setOversample (p.drive > 0.0f || p.resonance > 0.98f);
            freshNote = false;
        }

        // Glide/portamento: slew glideNote toward the target note over glideTime
        // (per render segment, so it's time-correct regardless of chunk size).
        if (p.glideTime <= 0.0005f)
            glideNote = (float) midiNote;
        else
            glideNote += (1.0f - std::exp (-(float) numSamples / (p.glideTime * (float) sampleRate)))
                         * ((float) midiNote - glideNote);

        // Filter/mod envelope -> pitch (control-rate: the env level at this chunk's
        // start scales the semitone offset, summed into the same pitch-mod domain as
        // the LFO). Sampled here so we don't advance the env twice; 16-sample chunks
        // give ~0.33 ms granularity — smooth for a kick's pitch drop. Default 0 -> 0.
        const float envPitchSemis = p.fltEnvToPitch * fltEnv.getLevel();

        // Mod matrix (#56): an ADDITIVE layer over the fixed LFO/env/velocity routes.
        // Evaluated once per control chunk (this render call is <= kSmoothChunk samples).
        // Per-part sources come from `partSrc`; per-voice ones are filled here. An inert
        // matrix yields zero offsets -> bit-identical render.
        ModMatrix::Offsets mm;
        if (mtx != nullptr && partSrc != nullptr && mtx->active())
        {
            ModSources src = *partSrc;
            src.modEnv   = fltEnv.getLevel();
            src.ampEnv   = ampEnv.getLevel();
            src.velocity = velocity;
            src.noteNorm = (float) (midiNote - 60) / 60.0f;
            src.random   = voiceRandom;
            mm = mtx->evaluate (src);
        }
        applyParams (p, envPitchSemis + mm.pitchSemis, mm.pw);

        const float trackOct = p.keytrack * (midiNote - 60) / 12.0f;
        const float velOct   = p.velToCutoff * velocity * 3.0f;              // vel -> cutoff
        const float ampScale = (1.0f - p.velToAmp) + p.velToAmp * velocity;  // vel -> amp
        const float ampMul   = std::clamp (1.0f + mm.amp, 0.0f, 2.0f);       // matrix -> amp

        // Effective per-source levels, folding in any matrix osc-level modulation. Used for
        // BOTH the kill-skip test and the mix, so a level modulated up from 0 is not skipped.
        const float l1 = std::clamp (p.osc1Level + mm.osc1Level, 0.0f, 1.0f);
        const float l2 = std::clamp (p.osc2Level + mm.osc2Level, 0.0f, 1.0f);
        const float l3 = std::clamp (p.osc3Level + mm.osc3Level, 0.0f, 1.0f);

        // Kill-skip: an oscillator whose (smoothed, on/off-folded) level is ~0 is
        // NOT rendered at all — the CPU saving the kill switch exists for, and
        // what makes osc3-off genuinely free. Level smoothing (engine side) means
        // this crosses the threshold while inaudibly quiet, so toggling is click-free.
        const bool o1 = l1 > 1.0e-4f;
        const bool o2 = l2 > 1.0e-4f;
        const bool o3 = l3 > 1.0e-4f;
        const bool useNoise = p.noiseLevel > 1.0e-4f;

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
                const float fc = p.cutoffHz * std::exp2 (envOct + trackOct + velOct + p.cutoffModOct + mm.cutoffOct);
                filter.setCutoff (fc, std::clamp (p.resonance + mm.reso, 0.0f, 1.0f));
            }

            // --- oscillators (per-source level; skip silent/off sources) ---
            float s = 0.0f;
            if (o1) s += osc1.nextSample() * l1;
            if (o2) s += osc2.nextSample() * l2;
            if (o3) s += osc3.nextSample() * l3;
            if (useNoise) s += noise() * p.noiseLevel;

            s = filter.process (s);
            out[i] += s * ampLevel * ampScale * ampMul * p.gain;   // p.gain == 1.0 for non-kit voices
        }
    }

private:
    // Filter-coefficient update interval (power of two for the bit mask).
    static constexpr int kCutoffInterval = 16;

    void applyParams (const VoiceParams& p, float extraPitchSemis = 0.0f, float extraPwMod = 0.0f)
    {
        // Pitch from the (glide-slewed) note plus pitch modulation (LFO + env->pitch).
        const double f0 = 440.0 * std::exp2 ((glideNote - 69.0f + p.pitchModSemis + extraPitchSemis) / 12.0);

        // Tier 1b: analog drift — a slow, bounded per-oscillator random walk on pitch (+ a hair of
        // PW), scaled by `analog`. HARD FAST PATH at analog<=0: no RNG, no math -> bit-exact + free.
        float d1c = 0.0f, d2c = 0.0f, d3c = 0.0f, dpw = 0.0f;
        if (p.analog > 0.0f)
        {
            auto walk = [this] (float& d)
            {
                driftRng ^= driftRng << 13; driftRng ^= driftRng >> 17; driftRng ^= driftRng << 5;
                const float r = (float) (std::int32_t) driftRng / 2147483648.0f;   // [-1,1)
                d = d * 0.999f + r * 0.012f;                    // leaky walk -> slow (~sub-Hz) wander
                d = d < -1.0f ? -1.0f : (d > 1.0f ? 1.0f : d);  // bounded
            };
            walk (drift1); walk (drift2); walk (drift3); walk (driftPw);
            const float cents = p.analog * kMaxDriftCents;      // +/- up to ~2 cents at analog = 1
            d1c = drift1 * cents; d2c = drift2 * cents; d3c = drift3 * cents;
            dpw = driftPw * p.analog * kMaxPwDrift;
        }

        osc1.setWave (static_cast<PolyBlepOscillator::Wave> (p.osc1Wave));
        osc2.setWave (static_cast<PolyBlepOscillator::Wave> (p.osc2Wave));
        osc3.setWave (static_cast<PolyBlepOscillator::Wave> (p.osc3Wave));
        osc1.setFrequency (f0 * std::exp2 (p.osc1Octave + (p.osc1Detune + d1c) / 1200.0f));
        osc2.setFrequency (f0 * std::exp2 (p.osc2Octave + (p.osc2Detune + d2c) / 1200.0f));
        osc3.setFrequency (f0 * std::exp2 (p.osc3Octave + (p.osc3Detune + d3c) / 1200.0f));
        osc1.setPulseWidth (std::clamp (p.osc1PW + p.pwMod + extraPwMod + dpw, 0.05f, 0.95f));
        osc2.setPulseWidth (std::clamp (p.osc2PW + p.pwMod + extraPwMod + dpw, 0.05f, 0.95f));
        osc3.setPulseWidth (std::clamp (p.osc3PW + p.pwMod + extraPwMod + dpw, 0.05f, 0.95f));

        filter.setType (static_cast<SVFilter::Type> (p.filterType));
        filter.setDrive (p.drive);                     // Tier 2: 0 -> bit-exact linear fast path
        ampEnv.setParameters (p.ampA, p.ampD, p.ampS, p.ampR);
        fltEnv.setParameters (p.fltA, p.fltD, p.fltS, p.fltR);
    }

    float noise()
    {
        // Fast xorshift white noise; good enough, allocation-free.
        nz ^= nz << 13; nz ^= nz >> 17; nz ^= nz << 5;
        return static_cast<float> (static_cast<std::int32_t> (nz)) / 2147483648.0f;
    }

    PolyBlepOscillator osc1, osc2, osc3;
    SVFilter           filter;
    ADSREnvelope       ampEnv, fltEnv;

    double sampleRate = 44100.0;
    int   midiNote  = 60;          // target note
    float glideNote = 60.0f;       // current (glide-slewed) note, fractional
    float velocity  = 0.0f;
    bool  active    = false;
    bool  generator = false;       // note came from a generator (seq/arp/loop) -> steal-first
    int   part      = 0;           // part index (7C): selects which params to render with
    int   soundSlot = 0;           // Kit pad index within the part (0 for non-kit voices)
    std::uint64_t timestamp = 0;   // for oldest-note stealing
    std::uint32_t nz = 0x12345678;
    float voiceRandom = 0.0f;      // per-note sample&hold (-1..1) — a mod-matrix source
    std::uint32_t rndState = 0x2545f491u;
    std::uint32_t phaseRng = 0x9e3779b9u;   // Tier 1: dedicated RNG so start-phase/drift don't perturb voiceRandom
    std::uint32_t driftRng = 0x1b56c4e9u;   // Tier 1b analog-drift RNG (only advances when analog > 0)
    float drift1 = 0.0f, drift2 = 0.0f, drift3 = 0.0f, driftPw = 0.0f;   // per-osc drift state (normalized ±1)
    static constexpr float kMaxDriftCents = 2.0f;   // pitch drift ceiling at analog = 1
    static constexpr float kMaxPwDrift    = 0.01f;  // a hair of pulse-width drift
    bool  freshNote = false;                        // Tier 2C: pending filter-oversampling latch for a new note

    // Start phase for a policy. RESET/FREE never draw an RNG, so the default (RESET) path is
    // bit-identical to before; only RANDOM consumes a value (a distinct stream from voiceRandom).
    double startPhaseFor (int mode)
    {
        if (mode == 1) { phaseRng ^= phaseRng << 13; phaseRng ^= phaseRng >> 17; phaseRng ^= phaseRng << 5;
                         return (double) phaseRng / 4294967296.0; }   // RANDOM in [0,1)
        return mode == 2 ? -1.0 : 0.0;                                 // FREE keeps phase; RESET -> 0
    }
};
