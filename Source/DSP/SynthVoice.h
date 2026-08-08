#pragma once
#include "PolyBlepOscillator.h"
#include "SVFilter.h"
#include "ADSREnvelope.h"
#include "ModMatrix.h"
#include <random>
#include <algorithm>
#include <cstdint>
#include <vector>
#include <array>

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
    float  osc1Semi   = 0, osc2Semi   = 0, osc3Semi   = 0;   // coarse tune, semitones (-24..+24)
    float  osc1Detune = 0, osc2Detune = 0, osc3Detune = 0;   // cents
    float  osc1PW = 0.5f, osc2PW = 0.5f, osc3PW = 0.5f;
    // #132 phase-modulation (FM) chain depth (0..1): osc2 phase-modulates osc1 (osc1Fm), osc3
    // modulates osc2 (osc2Fm). Applied only when the CARRIER wave is SIN/TRI/WT. Default 0 = off.
    float  osc1Fm = 0.0f, osc2Fm = 0.0f;
    // Musicality Tier 1a: per-oscillator start-phase policy (0 RESET / 1 RANDOM / 2 FREE).
    // Default 0 (RESET) keeps every note's waveform alignment bit-identical (goldens hold).
    int    osc1Phase = 0, osc2Phase = 0, osc3Phase = 0;
    // Tier 1b: analog drift amount (0..1) — one per part. 0 = bit-exact (no drift). Default 0.
    float  analog = 0.0f;
    // #95 Wavetable (Wave::Wavetable = 4): per-osc frame POSITION (0..1) and the SHARED, const table
    // this osc reads (owned by the engine's bank; null = the osc is silent in WT mode). Default 0 /
    // null keeps non-WT patches bit-identical.
    float  osc1WtPos = 0.0f, osc2WtPos = 0.0f, osc3WtPos = 0.0f;
    const Wavetable* osc1WtTable = nullptr;
    const Wavetable* osc2WtTable = nullptr;
    const Wavetable* osc3WtTable = nullptr;

    // mixer: independent per-source levels (engine writes SMOOTHED effective
    // levels here — already folded in the on/off kill switch, so a level of ~0
    // means "skip this oscillator"). oscMix stays as a legacy/no-op field.
    float  oscMix = 0.5f, noiseLevel = 0.0f;
    float  osc1Level = 0.8f, osc2Level = 0.8f, osc3Level = 0.0f;

    // velocity routing
    float  velToAmp    = 0.9f;                  // vel->amp depth: PERCEPTUAL (dB-linear) below unity (0.9: soft notes clearly quieter)
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

    // #96 Unison: N detuned + panned stack voices per note. 1 = OFF (the mono render path,
    // bit-identical). count > 1 renders N members (non-uniform detune, per-member random phase +
    // analog drift + stereo pan) through renderStereo(). detune 0..1 = cents spread, width 0..1 =
    // stereo spread. The count is LATCHED at note-on so the mono<->stereo path never switches mid-note.
    int    unisonCount   = 1;
    float  unisonDetune  = 0.15f;
    float  unisonWidth   = 0.5f;
};

class SynthVoice
{
public:
    static constexpr int kMaxUnison = 7;   // #96: max unison members (the engine caps the live count lower)

    // #132 FM (phase-modulation) chain. Carrier restriction: only continuous-phase waves —
    // Triangle(2)/Sine(3)/Wavetable(4) — can be phase-modulated cleanly; saw(0)/square(1) rely on
    // PolyBLEP edge corrections computed at the un-offset phase, which a phase offset invalidates.
    // Enum order matches PolyBlepOscillator::Wave.
    static bool fmCarrier (int wave) { return wave == 2 || wave == 3 || wave == 4; }
    static constexpr float kFmMaxDepth      = 1.0f;   // depth clamp (knob + matrix offset)
    static constexpr float kFmDepthToCycles = 1.0f;   // depth 1.0, full modulator -> +/-1 cycle phase (beta = 2*pi)

    // Set before prepare(): oscillator anti-aliasing quality.
    void setOscQuality (PolyBlepOscillator::Quality q)
    {
        oscQual = q;
        osc1.setQuality (q);
        osc2.setQuality (q);
        osc3.setQuality (q);
        for (auto& mem : uosc) for (auto& o : mem) o.setQuality (q);   // #96 unison members
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
        // #96 unison members: allocate the heap stack ONCE (message thread), apply quality + prepare.
        if (uosc.empty()) uosc.resize ((std::size_t) (kMaxUnison - 1));
        for (auto& mem : uosc) for (auto& o : mem) { o.setQuality (oscQual); o.prepare (newSampleRate); }
        filterR.prepare (newSampleRate);
    }

    void noteOn (int note, float vel, std::uint64_t stamp, int partIndex = 0, int slot = 0, bool gen = false,
                 int pm1 = 0, int pm2 = 0, int pm3 = 0,   // Tier 1a: per-osc start-phase policy (0 RESET default)
                 int unisonCnt = 1)                        // #96: unison count LATCHED here (mono<->stereo path)
    {
        unisonLatched = std::clamp (unisonCnt, 1, kMaxUnison);
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
            if (unisonLatched > 1 && ! uosc.empty())   // #96: members start with RANDOM phase (mandatory — equal phases collapse the stack)
            {
                for (int m = 0; m < unisonLatched - 1; ++m)
                    for (int k = 0; k < 3; ++k) uosc[(std::size_t) m][(std::size_t) k].reset (uRandPhase());
                for (auto& row : udrift) for (auto& d : row) d = 0.0f;
                filterR.reset();
            }
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
    bool isUnison() const  { return unisonLatched > 1; }   // #96: engine dispatches render vs renderStereo
    int  getNote() const   { return midiNote; }
    // Per-voice mod-matrix source values (for UI animation of env/vel/note/random routes on the
    // focused part — the engine samples the loudest live voice and publishes a representative snapshot).
    float getVelocity() const { return velocity; }
    float ampEnvLevel() const { return ampEnv.getLevel(); }
    float modEnvLevel() const { return fltEnv.getLevel(); }
    float getRandom()   const { return voiceRandom; }
    float noteNormVal() const { return (float) (midiNote - 60) / 60.0f; }
    int  getPart() const   { return part; }
    int  getSoundSlot() const { return soundSlot; }
    bool isGenerator() const { return generator; }
    // Sounding but key-up (amp env in its release tail). Voice stealing prefers these over
    // still-held voices so a held chord is never dropped while a fading voice exists (#141).
    bool isReleasing() const { return active && ampEnv.inRelease(); }
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
        applyParams (p, envPitchSemis + mm.pitchSemis, mm.pw, mm.wavePos);

        const float trackOct = p.keytrack * (midiNote - 60) / 12.0f;
        const float velOct   = p.velToCutoff * velocity * 3.0f;              // vel -> cutoff
        // vel -> amp, PERCEPTUAL (loudness is logarithmic): map velocity to amplitude in dB, so equal
        // velocity steps give equal loudness (dB) steps below unity; a gentle LINEAR boost handles
        // >1.0 accents (an exponential there would explode). velToAmp scales the depth (0 = inert),
        // velocity 1.0 = unity. std::pow once per render chunk is cheap.
        const float ampScale = std::max (kVelAmpFloor, (velocity <= 1.0f)
            ? std::pow (10.0f, p.velToAmp * (velocity - 1.0f) * (kVelAmpMaxDb / 20.0f))
            : 1.0f + p.velToAmp * (velocity - 1.0f) * kVelAccentGain);
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
        const float nl = std::clamp (p.noiseLevel + mm.noiseLevel, 0.0f, 1.0f);   // + mod-matrix noise level
        const bool useNoise = nl > 1.0e-4f;

        // #132 FM (phase-mod) chain osc3 -> osc2 -> osc1. Depth uses the modulator's RAW output
        // (independent of its mix level, so an inaudible modulator still shapes its carrier), scaled
        // to cycles of carrier phase; matrix mod (e.g. velocity -> FM) folds in here. Restricted to
        // SIN/TRI/WT carriers. All-zero (the default) leaves the call path bit-identical (goldens).
        const float fm1 = (fmCarrier (p.osc1Wave) ? std::clamp (p.osc1Fm + mm.osc1Fm, 0.0f, kFmMaxDepth) : 0.0f) * kFmDepthToCycles;  // osc2 -> osc1
        const float fm2 = (fmCarrier (p.osc2Wave) ? std::clamp (p.osc2Fm + mm.osc2Fm, 0.0f, kFmMaxDepth) : 0.0f) * kFmDepthToCycles;  // osc3 -> osc2
        const bool fm1On = fm1 != 0.0f, fm2On = fm2 != 0.0f;
        const bool r2 = o2 || fm1On;               // render osc2 if audible OR it modulates osc1
        const bool r3 = o3 || (fm2On && r2);       // render osc3 if audible OR it modulates a live osc2

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
            // Compute modulators first (osc3, then osc2) so their output offsets the carrier phase.
            const float m3 = r3 ? osc3.nextSample() : 0.0f;
            const float m2 = r2 ? osc2.nextSample (fm2On ? (double) (fm2 * m3) : 0.0) : 0.0f;
            const float m1 = o1 ? osc1.nextSample (fm1On ? (double) (fm1 * m2) : 0.0) : 0.0f;
            float s = 0.0f;
            if (o1) s += m1 * l1;
            if (o2) s += m2 * l2;
            if (o3) s += m3 * l3;
            if (useNoise) s += noise() * nl;

            s = filter.process (s);
            out[i] += s * ampLevel * ampScale * ampMul * p.gain;   // p.gain == 1.0 for non-kit voices
        }
    }

    // #96 Unison (count > 1 only): render N detuned + panned members into STEREO. Member 0 is
    // osc1/2/3 (the same oscillators); members 1..N-1 are uosc[]. Each member is a full 3-osc mix at
    // its own detune offset (non-uniform curve) + independent random phase (set at note-on) + its own
    // analog drift, panned across L/R by a 0 dB-centre balance law, through the L/R filter pair. A
    // 1/sqrt(N) trim keeps the level from jumping with the count. The mono render() above is untouched.
    void renderStereo (float* outL, float* outR, int numSamples, const VoiceParams& p,
                       const ModMatrix* mtx = nullptr, const ModSources* partSrc = nullptr)
    {
        if (! active) return;
        const int N = std::clamp (unisonLatched, 2, kMaxUnison);

        if (freshNote)
        {
            const bool os = p.drive > 0.0f || p.resonance > 0.98f;
            filter.setOversample (os); filterR.setOversample (os);
            freshNote = false;
        }

        if (p.glideTime <= 0.0005f) glideNote = (float) midiNote;
        else glideNote += (1.0f - std::exp (-(float) numSamples / (p.glideTime * (float) sampleRate)))
                          * ((float) midiNote - glideNote);

        const float envPitchSemis = p.fltEnvToPitch * fltEnv.getLevel();
        ModMatrix::Offsets mm;
        if (mtx != nullptr && partSrc != nullptr && mtx->active())
        {
            ModSources src = *partSrc;
            src.modEnv = fltEnv.getLevel(); src.ampEnv = ampEnv.getLevel(); src.velocity = velocity;
            src.noteNorm = (float) (midiNote - 60) / 60.0f; src.random = voiceRandom;
            mm = mtx->evaluate (src);
        }
        configureUnison (p, N, envPitchSemis + mm.pitchSemis, mm.pw, mm.wavePos);

        // Per-member pan weights (0 dB-centre balance law, matching the part pan) + the level trim.
        float panL[kMaxUnison], panR[kMaxUnison];
        for (int mIdx = 0; mIdx < N; ++mIdx)
        {
            const float pos = unisonPos (mIdx, N) * p.unisonWidth;
            panL[mIdx] = pos <= 0.0f ? 1.0f : 1.0f - pos;
            panR[mIdx] = pos >= 0.0f ? 1.0f : 1.0f + pos;
        }
        const float uGain = 1.0f / std::sqrt ((float) N);

        const float trackOct = p.keytrack * (midiNote - 60) / 12.0f;
        const float velOct   = p.velToCutoff * velocity * 3.0f;
        const float ampScale = std::max (kVelAmpFloor, (velocity <= 1.0f)
            ? std::pow (10.0f, p.velToAmp * (velocity - 1.0f) * (kVelAmpMaxDb / 20.0f))
            : 1.0f + p.velToAmp * (velocity - 1.0f) * kVelAccentGain);
        const float ampMul   = std::clamp (1.0f + mm.amp, 0.0f, 2.0f);

        const float l1 = std::clamp (p.osc1Level + mm.osc1Level, 0.0f, 1.0f);
        const float l2 = std::clamp (p.osc2Level + mm.osc2Level, 0.0f, 1.0f);
        const float l3 = std::clamp (p.osc3Level + mm.osc3Level, 0.0f, 1.0f);
        const bool o1 = l1 > 1.0e-4f, o2 = l2 > 1.0e-4f, o3 = l3 > 1.0e-4f;
        const float nl = std::clamp (p.noiseLevel + mm.noiseLevel, 0.0f, 1.0f);
        const bool useNoise = nl > 1.0e-4f;

        // #132 FM chain (per stack-voice pair — each member's osc3->osc2->osc1, never across members).
        const float fm1 = (fmCarrier (p.osc1Wave) ? std::clamp (p.osc1Fm + mm.osc1Fm, 0.0f, kFmMaxDepth) : 0.0f) * kFmDepthToCycles;
        const float fm2 = (fmCarrier (p.osc2Wave) ? std::clamp (p.osc2Fm + mm.osc2Fm, 0.0f, kFmMaxDepth) : 0.0f) * kFmDepthToCycles;
        const bool fm1On = fm1 != 0.0f, fm2On = fm2 != 0.0f;
        const bool r2 = o2 || fm1On;
        const bool r3 = o3 || (fm2On && r2);

        for (int i = 0; i < numSamples; ++i)
        {
            const float ampLevel = ampEnv.nextSample();
            const float fltLevel = fltEnv.nextSample();
            if (! ampEnv.isActive()) { active = false; break; }

            if ((i & (kCutoffInterval - 1)) == 0)
            {
                const float envOct = p.filterEnvAmt * fltLevel * 5.0f;
                const float fc = p.cutoffHz * std::exp2 (envOct + trackOct + velOct + p.cutoffModOct + mm.cutoffOct);
                const float rz = std::clamp (p.resonance + mm.reso, 0.0f, 1.0f);
                filter.setCutoff (fc, rz); filterR.setCutoff (fc, rz);
            }

            float accL = 0.0f, accR = 0.0f;
            {   // member 0 = osc1/osc2/osc3 (not treated as an array — no contiguity assumption)
                const float m3 = r3 ? osc3.nextSample() : 0.0f;
                const float m2 = r2 ? osc2.nextSample (fm2On ? (double) (fm2 * m3) : 0.0) : 0.0f;
                const float m1 = o1 ? osc1.nextSample (fm1On ? (double) (fm1 * m2) : 0.0) : 0.0f;
                float sm = 0.0f;
                if (o1) sm += m1 * l1;
                if (o2) sm += m2 * l2;
                if (o3) sm += m3 * l3;
                if (useNoise) sm += noise() * nl;                // noise stays centred (with member 0)
                accL += sm * panL[0]; accR += sm * panR[0];
            }
            for (int mIdx = 1; mIdx < N; ++mIdx)
            {
                auto& os = uosc[(std::size_t) (mIdx - 1)];
                const float m3 = r3 ? os[2].nextSample() : 0.0f;
                const float m2 = r2 ? os[1].nextSample (fm2On ? (double) (fm2 * m3) : 0.0) : 0.0f;
                const float m1 = o1 ? os[0].nextSample (fm1On ? (double) (fm1 * m2) : 0.0) : 0.0f;
                float sm = 0.0f;
                if (o1) sm += m1 * l1;
                if (o2) sm += m2 * l2;
                if (o3) sm += m3 * l3;
                accL += sm * panL[mIdx];
                accR += sm * panR[mIdx];
            }
            accL = filter.process (accL);
            accR = filterR.process (accR);
            const float g = ampLevel * ampScale * ampMul * p.gain * uGain;
            outL[i] += accL * g;
            outR[i] += accR * g;
        }
    }

private:
    // Symmetric NON-uniform member position in [-1,1] (denser toward the centre) — no transcendental,
    // so it's cross-platform clean. j in [0, N-1]; the centre member (odd N) sits at 0.
    static float unisonPos (int j, int N)
    {
        if (N <= 1) return 0.0f;
        const float x = 2.0f * (float) j / (float) (N - 1) - 1.0f;   // even spacing in [-1,1]
        return x * (0.6f + 0.4f * x * x);                            // mild cubic non-uniformity
    }

    // Configure member 0 (via the untouched applyParams) + members 1..N-1 (detune offset + own drift).
    void configureUnison (const VoiceParams& p, int N, float extraPitchSemis, float extraPwMod, float wavePosMod)
    {
        applyParams (p, extraPitchSemis, extraPwMod, wavePosMod);   // member 0 = osc1/2/3 (base pitch)
        const double f0 = 440.0 * std::exp2 ((glideNote - 69.0f + p.pitchModSemis + extraPitchSemis) / 12.0);
        const int   waves[3] { p.osc1Wave, p.osc2Wave, p.osc3Wave };
        const float octs[3]  { p.osc1Octave, p.osc2Octave, p.osc3Octave };
        const float dets[3]  { p.osc1Detune, p.osc2Detune, p.osc3Detune };
        const float pws[3]   { p.osc1PW, p.osc2PW, p.osc3PW };
        const float wpos[3]  { p.osc1WtPos, p.osc2WtPos, p.osc3WtPos };
        const Wavetable* wts[3] { p.osc1WtTable, p.osc2WtTable, p.osc3WtTable };
        const float driftCents = p.analog * kMaxDriftCents;
        for (int m = 0; m < N - 1; ++m)
        {
            const float memberCents = unisonPos (m + 1, N) * p.unisonDetune * kMaxUnisonCents;
            for (int k = 0; k < 3; ++k)
            {
                auto& o = uosc[m][k];
                o.setWave (static_cast<PolyBlepOscillator::Wave> (waves[k]));
                o.setWavetable (wts[k]);
                float dc = 0.0f;
                if (p.analog > 0.0f)
                {
                    uDriftRng ^= uDriftRng << 13; uDriftRng ^= uDriftRng >> 17; uDriftRng ^= uDriftRng << 5;
                    const float r = (float) (std::int32_t) uDriftRng / 2147483648.0f;
                    float& d = udrift[m][k];
                    d = d * 0.999f + r * 0.012f;
                    d = d < -1.0f ? -1.0f : (d > 1.0f ? 1.0f : d);
                    dc = d * driftCents;
                }
                o.setFrequency (f0 * std::exp2 (octs[k] + (dets[k] + dc + memberCents) / 1200.0f));
                o.setPulseWidth (std::clamp (pws[k] + p.pwMod + extraPwMod, 0.05f, 0.95f));
                o.setWavePosition (wpos[k] + wavePosMod);
            }
        }
    }

    // A random start phase in [0,1) for a unison member osc (distinct RNG stream; only ever advanced
    // when unison > 1, so a non-unison voice's RNG streams — hence its output — are unchanged).
    double uRandPhase()
    {
        uPhaseRng ^= uPhaseRng << 13; uPhaseRng ^= uPhaseRng >> 17; uPhaseRng ^= uPhaseRng << 5;
        return (double) uPhaseRng / 4294967296.0;
    }

    // Filter-coefficient update interval (power of two for the bit mask).
    static constexpr int kCutoffInterval = 16;

    void applyParams (const VoiceParams& p, float extraPitchSemis = 0.0f, float extraPwMod = 0.0f,
                      float wavePosMod = 0.0f)
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
        // WT source BEFORE setFrequency (the mip is picked from the pitch against the current table).
        osc1.setWavetable (p.osc1WtTable);
        osc2.setWavetable (p.osc2WtTable);
        osc3.setWavetable (p.osc3WtTable);
        osc1.setFrequency (f0 * std::exp2 (p.osc1Octave + p.osc1Semi / 12.0f + (p.osc1Detune + d1c) / 1200.0f));
        osc2.setFrequency (f0 * std::exp2 (p.osc2Octave + p.osc2Semi / 12.0f + (p.osc2Detune + d2c) / 1200.0f));
        osc3.setFrequency (f0 * std::exp2 (p.osc3Octave + p.osc3Semi / 12.0f + (p.osc3Detune + d3c) / 1200.0f));
        osc1.setPulseWidth (std::clamp (p.osc1PW + p.pwMod + extraPwMod + dpw, 0.05f, 0.95f));
        osc2.setPulseWidth (std::clamp (p.osc2PW + p.pwMod + extraPwMod + dpw, 0.05f, 0.95f));
        osc3.setPulseWidth (std::clamp (p.osc3PW + p.pwMod + extraPwMod + dpw, 0.05f, 0.95f));
        // WT frame position: per-osc base + the shared WavePos mod (voice-tier), smoothed in the osc.
        osc1.setWavePosition (p.osc1WtPos + wavePosMod);
        osc2.setWavePosition (p.osc2WtPos + wavePosMod);
        osc3.setWavePosition (p.osc3WtPos + wavePosMod);

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

    PolyBlepOscillator osc1, osc2, osc3;                 // unison member 0 (the bit-exact mono path)
    SVFilter           filter;
    ADSREnvelope       ampEnv, fltEnv;

    // #96 Unison: members 1..N-1 (member 0 is osc1/2/3 above). Only touched when unison > 1, so a
    // non-unison voice never allocates work here and stays bit-identical. filterR is the right
    // channel for the stereo unison path.
    static constexpr float kMaxUnisonCents = 50.0f;      // ± cents spread at unisonDetune = 1
    // HEAP-allocated (sized in prepare, message thread) so the voice — and the engine's 24-voice
    // array — stay small on the stack. 18 extra oscillators by value would blow a stacked engine.
    std::vector<std::array<PolyBlepOscillator, 3>> uosc;
    PolyBlepOscillator::Quality oscQual = PolyBlepOscillator::Quality::Efficient;   // applied to uosc at prepare
    SVFilter           filterR;
    float              udrift[kMaxUnison - 1][3] {};     // per-member analog drift state
    std::uint32_t      uPhaseRng = 0x85ebca6bu;          // member start-phase RNG (unison > 1 only)
    std::uint32_t      uDriftRng = 0xc2b2ae35u;          // member drift RNG (unison > 1 only)
    int                unisonLatched = 1;                // count latched at note-on

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
    static constexpr float kVelAmpMaxDb   = 40.0f;  // vel->amp perceptual depth (dB) at velToAmp = 1
    static constexpr float kVelAccentGain = 1.0f;   // gentle linear amp boost per unit velocity above 1.0
    // Floor under the vel->amp scale: the softest hit never drops below this (~-18 dB). Keeps the
    // FULL upper dynamic range (a hard hit is untouched) but stops a light/uneven touch — common in
    // fast passages — from falling so far it reads as a dropped note. Raise for more presence on
    // soft hits, lower for a deeper pianissimo.
    static constexpr float kVelAmpFloor   = 0.125f; // -18.06 dB; softest note is >= 1/8 of a full hit
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
