#pragma once
#include "SynthVoice.h"
#include "SampleVoice.h"
#include "Kit.h"
#include "LFO.h"
#include "FXChain.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

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

// Per-part LFO configuration (Sub-phase 2). Three LFOs per part; each routes to a
// single destination (0 off / 1 pitch / 2 cutoff / 3 PW). Depth 0 or dest 0 = inert.
struct LfoConfig { float rate = 2.0f, depth = 0.0f; int shape = 0, dest = 0;
                   bool synced = false; int division = 5; };   // J1: sync + note-division index
struct PartLfos  { LfoConfig lfo[3]; };

// J1: LFO note divisions -> cycle length in BEATS (4/4). Index order matches the lfo_div param
// choices { 4 bar,2 bar,1/1,1/2,1/4,1/8,1/16,1/32, 1/4T,1/8T,1/16T, 1/4.,1/8.,1/16. }.
namespace lfodiv
{
    inline constexpr int kNum = 14;
    inline constexpr double kBeats[kNum] =
        { 16.0, 8.0, 4.0, 2.0, 1.0, 0.5, 0.25, 0.125,           // 4bar..1/32
          2.0/3.0, 1.0/3.0, 1.0/6.0,                            // 1/4T, 1/8T, 1/16T
          1.5, 0.75, 0.375 };                                   // 1/4., 1/8., 1/16.
    inline double cycleBeats (int i) { return kBeats[(i >= 0 && i < kNum) ? i : 4]; }
}

class SynthEngine
{
public:
    static constexpr int maxVoices  = 24;   // pool size (raised for multitimbral: seq + kit + lead + more)
    static constexpr int kTrimVoices = 16;  // voice-sum trim reference (kept at 16 so the pool size
                                            // doesn't change single-note level or the goldens)

    void prepare (double newSampleRate, int maxBlock = 2048)
    {
        sampleRate = newSampleRate;
        maxBlockSize = maxBlock;
        for (auto& v : voices)
        {
            v.setOscQuality (oscQuality);
            v.prepare (sampleRate);
        }
        for (auto& sv : sampleVoices) sv.prepare (sampleRate);   // I2: stereo sample-pad voices
        // Per-part FX (Sub-phase 2): each part owns a chain + stereo scratch, all sized
        // now so renderMaster() never allocates. Skipped for silent/bypassed parts.
        for (int p = 0; p < maxParts; ++p)
        {
            partFx[(std::size_t) p].prepare (sampleRate, maxBlock);
            partMono[(std::size_t) p].assign ((std::size_t) maxBlock, 0.0f);
            partL[(std::size_t) p].assign ((std::size_t) maxBlock, 0.0f);
            partR[(std::size_t) p].assign ((std::size_t) maxBlock, 0.0f);
            partSampleL[(std::size_t) p].assign ((std::size_t) maxBlock, 0.0f);   // I2: per-part sample bus
            partSampleR[(std::size_t) p].assign ((std::size_t) maxBlock, 0.0f);
            fxSilentBlocks[(std::size_t) p] = kFxHoldBlocks;   // start idle
        }
        for (std::size_t p = 0; p < (std::size_t) maxParts; ++p)   // per-part looper capture taps
        {
            capPartL[p].assign ((std::size_t) maxBlock, 0.0f);
            capPartR[p].assign ((std::size_t) maxBlock, 0.0f);
        }
        lfo.prepare (sampleRate);
        for (auto& part : partLfo) for (auto& l : part) l.prepare (sampleRate);   // 3 LFOs x maxParts
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
        // Fixed voice-sum headroom trim, decoupled from the POOL SIZE so raising maxVoices
        // (for multitimbral polyphony) never quietens single notes or shifts goldens. The
        // trim is 1/sqrt(kTrimVoices) — the equal-power sum rule at a fixed nominal
        // simultaneous-voice count — applied ONCE to the mono sum (never per active-voice
        // count, which pumps). Typical playing stays well under full-scale; denser-than-
        // nominal chords are caught by the processor's safety clipper.
        voiceTrim = 1.0f / std::sqrt ((float) kTrimVoices);
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

    // Poly (0) / Mono (1) / Legato (2) — PER PART. Switching a part's mode releases only
    // THAT part's voices + clears its note stack, so no note is stranded and other parts
    // are untouched. Kit parts are forced poly (see modeForPart()).
    void setPartPolyMode (int part, int mode)
    {
        if (part < 0 || part >= maxParts) return;
        if (mode != partPolyMode[(std::size_t) part])
        {
            numMonoHeld[(std::size_t) part] = 0;
            monoVoice[(std::size_t) part] = -1;
            for (std::size_t i = 0; i < (std::size_t) maxVoices; ++i)
                if (voices[i].isActive() && voices[i].getPart() == part) { voices[i].noteOff(); sustained[i] = false; }
        }
        partPolyMode[(std::size_t) part] = mode;
    }

    // The effective mode for a part: kits are ALWAYS poly (drums are polyphonic).
    int modeForPart (int part) const
    {
        if (part < 0 || part >= maxParts || partIsKit (part)) return 0;
        return partPolyMode[(std::size_t) part];
    }

    // ---- MIDI (called from processBlock with sample-accurate offsets) -----
    // `part` selects which part's params the voice renders with (7C). The 16-voice
    // pool is SHARED across parts with global oldest-note stealing (per-part
    // reservation is future work). Voice identity is (note, part) so two parts can
    // hold the same note number without over-releasing.
    void noteOn (int note, float velocity, int part = 0, int soundSlot = 0, bool generator = false)
    {
        if (modeForPart (part) != 0) { monoNoteOn (note, velocity, part, soundSlot, generator); return; }

        // Tier 1a: this part+pad's per-oscillator start-phase policy (only a fresh voice acts on it).
        const auto& pp = paramsFor (part, soundSlot);
        const int pm1 = pp.osc1Phase, pm2 = pp.osc2Phase, pm3 = pp.osc3Phase;

        // Reuse a voice already playing this (note, part) — retrigger in place (keeps
        // oscillator/filter state; the amp env re-attacks from its current level).
        //
        // EXCEPT for a PERCUSSIVE sound — an amp env with no sustain, i.e. a drum. There,
        // re-attacking in place is an audible "double-hit pop": the amp re-attack corner
        // AND the mod-env pitch restart (e.g. Kick 808's +22 st sweep) both land as slope
        // discontinuities against the still-sounding tail. Instead, fade the old tail with
        // a quick release (~4 ms) and let the new hit start from SILENCE on a fresh voice
        // below (phase 0, envelopes from 0) — a clean, drum-machine-style re-strike whose
        // brief overlap with the fading tail is smooth. All matching tails are faded so a
        // fast roll can't pile up voices.
        const bool percussive = pp.ampS < 0.02f;
        for (std::size_t i = 0; i < activeVoiceLimit; ++i)
            if (voices[i].isActive() && voices[i].getNote() == note && voices[i].getPart() == part)
            {
                sustained[i] = false;
                if (percussive) { voices[i].steal(); continue; }   // fade tail, fall through to a fresh voice
                voices[i].noteOn (note, velocity, ++eventCounter, part, soundSlot, generator, pm1, pm2, pm3);
                return;
            }

        // Otherwise find a free voice...
        for (std::size_t i = 0; i < activeVoiceLimit; ++i)
            if (! voices[i].isActive())
                { sustained[i] = false; voices[i].noteOn (note, velocity, ++eventCounter, part, soundSlot, generator, pm1, pm2, pm3); return; }

        // ...or steal a voice. PER-PART ISOLATION, in priority order:
        //   1. the oldest GENERATOR voice (seq / arp / looper) — generators ALWAYS yield to
        //      live playing, so a running sequencer can never cut a note you play.
        //   2. else the oldest voice OF THIS part (live-vs-live stealing stays inside the part).
        //   3. else the global oldest (last resort: the pool is exhausted by other live parts).
        // The stolen voice keeps its oscillator/filter state and retriggers the amp env from
        // its current level, so the steal is click-free.
        int oldestGen = -1, oldestOwn = -1;
        std::size_t oldestAny = 0;
        for (std::size_t i = 0; i < activeVoiceLimit; ++i)
        {
            if (voices[i].isGenerator()
                && (oldestGen < 0 || voices[i].getTimestamp() < voices[(std::size_t) oldestGen].getTimestamp()))
                oldestGen = (int) i;
            if (voices[i].getPart() == part
                && (oldestOwn < 0 || voices[i].getTimestamp() < voices[(std::size_t) oldestOwn].getTimestamp()))
                oldestOwn = (int) i;
            if (voices[i].getTimestamp() < voices[oldestAny].getTimestamp())
                oldestAny = i;
        }
        const std::size_t steal = (oldestGen >= 0) ? (std::size_t) oldestGen
                                : (oldestOwn >= 0) ? (std::size_t) oldestOwn : oldestAny;

        sustained[steal] = false;
        ++stealCounter;
        voices[steal].noteOn (note, velocity, ++eventCounter, part, soundSlot, generator, pm1, pm2, pm3);
    }

    // ---- observability accessors (const; for the processor's telemetry) ----
    int activeVoiceCount() const
    {
        int c = 0;
        for (auto& v : voices) if (v.isActive()) ++c;
        return c;
    }
    int activeVoiceCountForPart (int part) const
    {
        int c = 0;
        for (auto& v : voices) if (v.isActive() && v.getPart() == part) ++c;
        return c;
    }
    std::uint64_t stealCount() const { return stealCounter; }

    void noteOff (int note, int part = 0)
    {
        if (modeForPart (part) != 0) { monoNoteOff (note, part); return; }

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
        for (auto& sv : sampleVoices) sv.steal();               // I2: fade out any playing samples (click-free)
        sustainPedal = false;
        for (std::size_t p = 0; p < (std::size_t) maxParts; ++p) { numMonoHeld[p] = 0; monoVoice[p] = -1; }
        for (auto& e : kitLedger) { e.part = -1; e.trigger = -1; e.num = 0; }
    }

    // Release every voice sounding on `part` (1.3: clean hand-off when the edit focus /
    // live part changes, so a note played on the old part can't get stranded when its
    // note-off later routes to the new part). Voices fade via their release envelope.
    void releasePartNotes (int part)
    {
        for (std::size_t i = 0; i < (std::size_t) maxVoices; ++i)
            if (voices[i].isActive() && voices[i].getPart() == part) { voices[i].noteOff(); sustained[i] = false; }
        for (auto& e : kitLedger) if (e.part == part) { e.part = -1; e.trigger = -1; e.num = 0; }
        if (part >= 0 && part < maxParts) { numMonoHeld[(std::size_t) part] = 0; monoVoice[(std::size_t) part] = -1; }
    }

    // ---- performance controllers (from any device) -------------------------
    void setPitchBend (float semitones) { pitchBendSemis.fill (semitones); }   // all parts (host/global wheel)
    void setModWheel   (float amount01) { modWheel.fill (amount01); }          // -> vibrato depth, all parts
    void setPitchBend (int part, float semitones) { if (part >= 0 && part < maxParts) pitchBendSemis[(std::size_t) part] = semitones; }
    void setModWheel   (int part, float amount01) { if (part >= 0 && part < maxParts) modWheel[(std::size_t) part]     = amount01; }
    // Read-back for the F12 diagnostic + tests: the largest-magnitude bend / mod across parts
    // ("is anything bending?"), plus a per-part overload.
    float pitchBendSemitones() const { float m = 0.0f; for (auto v : pitchBendSemis) if (std::abs (v) > std::abs (m)) m = v; return m; }
    float modWheelAmount()     const { float m = 0.0f; for (auto v : modWheel) m = std::max (m, v); return m; }
    float pitchBendSemitones (int part) const { return (part >= 0 && part < maxParts) ? pitchBendSemis[(std::size_t) part] : 0.0f; }
    float modWheelAmount     (int part) const { return (part >= 0 && part < maxParts) ? modWheel[(std::size_t) part]     : 0.0f; }

    // Mod matrix (#56): the FOCUSED part's live routing table + the current 8 macro values
    // (matrix sources). Called by the processor before beginMasterBlock each block.
    void setLiveModMatrix (const ModMatrix& m)        { liveMatrixStore = m; }
    void setMacroValues   (const std::array<float, 8>& m) { macroVals = m; }
    // J1: the transport beat position at this block's start + samples-per-beat, for tempo-synced LFOs.
    void setTransport (double beats, double spb) { transportBeats_ = beats; samplesPerBeat_ = spb > 0.0 ? spb : 1.0; }

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

    // Publish a locked part's baked voice params + its FX + its 3 LFOs (message thread;
    // lock-free double-buffer, audio thread reads the current slot each block). The FX
    // and LFOs travel WITH the voice params so a locked part is fully self-contained and
    // there is no per-part FX/LFO data race with the audio thread (Sub-phase 2).
    void setLockedPartParams (int part, const VoiceParams& vp,
                              const FXParams& fx = {}, const PartLfos& lfo = {}, const ModMatrix& mtx = {})
    {
        // part 0 is publishable too now: when the edit focus moves OFF part 0, its last
        // state is baked into slot 0 like any other locked part (1.3 edit-focus).
        if (part >= 0 && part < maxParts)
        {
            lockedSlots[(std::size_t) part].publish ({ vp, fx, lfo, mtx });
            setPartPolyMode (part, vp.polyMode);   // a locked part bakes its own poly/mono/legato
        }
    }

    // ---- kit parts ---------------------------------------------------------
    // Publish a KIT for a part (message thread; double-buffered like a locked part).
    // isKit=false collapses it back to a plain locked/live part.
    void setPartKit (int part, const KitData& k)
    {
        if (part >= 1 && part < maxParts) kitSlots[(std::size_t) part].publish (k);
    }
    void clearPartKit (int part)
    {
        if (part >= 1 && part < maxParts) { KitData off; kitSlots[(std::size_t) part].publish (off); }
    }
    bool partIsKit (int part) const
    {
        return part >= 0 && part < maxParts && kitSlots[(std::size_t) part].current().isKit;
    }

    // Trigger a kit pad: expand to its sounding notes (each rendered with the pad's
    // baked params via soundSlot), applying choke. Unmapped trigger = silence. Called
    // on the audio thread (drain / host loop), like chord expansion.
    void kitNoteOn (int part, int trigger, float velocity, bool generator = false)
    {
        if (part < 1 || part >= maxParts) return;
        const KitData& k = kitSlots[(std::size_t) part].current();
        const int pad = k.padForTrigger (trigger);
        if (pad < 0) return;                                    // no pad here -> silence

        const int group = k.pads[(std::size_t) pad].chokeGroup;
        if (group != 0)                                         // cross-pad choke: quick-release the group
        {                                                       // across BOTH the synth and sample pools (I2)
            for (auto& v : voices)
                if (v.isActive() && v.getPart() == part && v.getSoundSlot() != pad
                    && k.pads[(std::size_t) v.getSoundSlot()].chokeGroup == group)
                    v.steal();
            for (auto& sv : sampleVoices)
                if (sv.isActive() && sv.getPart() == part && sv.getSoundSlot() != pad
                    && k.pads[(std::size_t) sv.getSoundSlot()].chokeGroup == group)
                    sv.steal();
        }

        auto* e = kitLedgerFor (part, trigger, true);
        if (e != nullptr) { e->num = 0; e->slot = pad; }
        const auto& pd = k.pads[(std::size_t) pad];
        for (int i = 0; i < pd.numSound && i < kMaxPadSoundNotes; ++i)
        {
            if (pd.isSample)
                sampleNoteOn (pd, pd.soundNote[(std::size_t) i], velocity, part, pad, generator);
            else
                noteOn (pd.soundNote[(std::size_t) i], velocity, part, pad, generator);   // self-choke = retrigger same voice
            if (e != nullptr && e->num < kMaxPadSoundNotes) e->notes[(std::size_t) e->num++] = pd.soundNote[(std::size_t) i];
        }
    }

    // Allocate a stereo sample voice for a sample pad (I2). Poly (each hit layers); steals the
    // oldest sample voice when the pool is full. Choke/note-off are handled like synth pads.
    void sampleNoteOn (const KitPad& pd, int note, float velocity, int part, int pad, bool gen)
    {
        SampleVoice* pick = nullptr;
        for (auto& sv : sampleVoices) if (! sv.isActive()) { pick = &sv; break; }
        if (pick == nullptr)                                    // steal the oldest sounding sample voice
        {
            std::uint64_t oldest = ~0ull;
            for (auto& sv : sampleVoices) if (sv.stamp() < oldest) { oldest = sv.stamp(); pick = &sv; }
        }
        if (pick == nullptr) return;
        SamplePlay sp { pd.sampleL, pd.sampleR, pd.sampleLen, pd.sampleSR, pd.sampleRoot,
                        pd.sampleGain * velocity };
        pick->noteOn (sp, note, ++eventCounter, part, pad, gen);
    }
    void kitNoteOff (int part, int trigger)
    {
        auto* e = kitLedgerFor (part, trigger, false);
        if (e == nullptr) return;
        for (int i = 0; i < e->num; ++i) noteOff (e->notes[(std::size_t) i], part);
        e->part = -1; e->trigger = -1; e->num = 0;             // free the slot
    }

    // The single per-voice params-selection SEAM. `slot` = the voice's sound slot
    // (Kit pad index; 0 for non-kit voices). A locked/live part ignores it; a Kit part
    // returns that pad's baked params. Uses the per-block-snapshotted read index so a
    // mid-block publish can't tear params across chunks.
    const VoiceParams& paramsFor (int part, int slot) const
    {
        const int p = (part >= 0 && part < maxParts) ? part : 0;
        // Kit pad edit (Group 4): the ONE pad being edited plays the live panel params
        // (partParams[liveIndex]) while every other pad keeps its baked sound, so the rest
        // of the kit plays normally under the sequencer while you sculpt the one.
        if (p == liveKitPart && slot == liveKitPad) return partParams[(std::size_t) liveIndex];
        const auto& kb = kitSlots[(std::size_t) p].buf[(std::size_t) kitReadIdx[(std::size_t) p]];
        if (kb.isKit) return kb.params[(std::size_t) ((slot >= 0 && slot < kMaxKitPads) ? slot : 0)];
        return partParams[(std::size_t) p];
    }

    // Kit pad edit: route ONE kit pad's voice through the live panel params (‑1 = none).
    void setLiveKitPad (int part, int pad) { liveKitPart = part; liveKitPad = pad; }

    // ---- rendering ---------------------------------------------------------
    // Renders MONO into `out`; the processor copies to both channels. `liveParams`
    // is part 0 (the LIVE part, smoothed here). Locked parts render from their
    // baked params. The global LFO / bend / vibrato are SHARED by all parts (v1).
    void render (float* out, int numSamples, VoiceParams liveParams,
                 float lfoRate, int lfoShape, float lfoDepth, int lfoDest)
    {
        lfo.setRate (lfoRate);
        lfo.setShape (static_cast<LFO::Shape> (lfoShape));

        // Snapshot the locked parts' baked params + each part's kit read-index for this
        // render (lock-free). The index is sampled once so a mid-block kit publish can't
        // tear a voice's params across chunks.
        for (int pt = 0; pt < maxParts; ++pt)
            kitReadIdx[(std::size_t) pt] = kitSlots[(std::size_t) pt].idx.load (std::memory_order_acquire);
        for (int pt = 1; pt < maxParts; ++pt)
            partParams[(std::size_t) pt] = lockedSlots[(std::size_t) pt].current().vp;

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
            const float vibRaw = vibratoLFO.advance (chunk);   // advance once per chunk; scale per part below

            // Each active voice renders with ITS part's params + the shared mods + that part's
            // own pitch-bend/vibrato (per-part performance controllers, G6).
            for (auto& v : voices)
            {
                if (! v.isActive()) continue;
                const int pt = v.getPart();
                VoiceParams p = paramsFor (pt, v.getSoundSlot());
                const float bendVib = pitchBendSemis[(std::size_t) pt] + vibRaw * modWheel[(std::size_t) pt] * kVibratoSemis;
                p.pitchModSemis += modPitch + bendVib;      // base pitchModSemis is 0
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

    // ---- multitimbral master render (Sub-phase 2) --------------------------
    // Full STEREO master: each part's voices render into that part's own buffer, run
    // through that part's own FX chain, then sum (unity — per-part level/pan is a
    // deferred mixer). A part with no active voices AND an idle FX chain is SKIPPED
    // (the CPU control; a decaying FX tail keeps processing until silent). part 0 is
    // the smoothed LIVE part; the shared LFO/bend/vibrato drive every part (per-part
    // LFOs land next). Split into begin/renderParts/mixParts so the processor can keep
    // host MIDI sample-accurate (render voices per event segment) while FX+sum run once
    // per block. renderMaster() is the whole-block convenience for tests.

    // A copy of `src` with only the per-part EQ kept (all creative FX disabled). Used to give
    // kit parts their channel EQ while keeping chorus/delay/reverb/width dry (v1).
    static FXParams eqOnly (const FXParams& src)
    {
        FXParams f;                                       // ctor: all enabled[] = false
        f.eqBand1 = src.eqBand1; f.eqBand2 = src.eqBand2; f.eqBand3 = src.eqBand3;
        f.eqBand4 = src.eqBand4; f.eqBand5 = src.eqBand5;
        f.enabled[FXChain::EQ_] = src.enabled[FXChain::EQ_];
        return f;
    }

    // Clear per-part accumulators + snapshot locked/kit state for the block. part 0 uses
    // the caller's LIVE FX + LFOs; parts 1-3 use the FX/LFOs published WITH their baked
    // voice params (a kit part is dry). partFxUse/partLfoUse become the per-part config
    // for renderParts/mixParts — no data race with the message thread.
    // `focus` = the LIVE/edited part (1.3): its params come from liveParams (APVTS,
    // smoothed); every OTHER part plays from its baked locked slot. focus == 0 is the
    // historical behaviour (part 0 live, 1-3 baked) — bit-identical.
    void beginMasterBlock (int numSamples, VoiceParams liveParams,
                           const FXParams& liveFx, const PartLfos& liveLfo, int focus = 0)
    {
        if (focus < 0 || focus >= maxParts) focus = 0;
        if (focus != liveIndex) smoothPrimed = false;    // re-prime smoothing to the new focus
        liveIndex = focus;

        for (int pt = 0; pt < maxParts; ++pt)
            kitReadIdx[(std::size_t) pt] = kitSlots[(std::size_t) pt].idx.load (std::memory_order_acquire);

        // The focused part uses the panel's FX/LFO — unless it's a kit. Kits keep the creative
        // FX (chorus/delay/reverb/width) DRY in v1 so a pad edit never FXes the other pads, but
        // the per-part EQ IS a part-bus shaper (applied to the whole kit's summed output, not
        // per-pad), so it applies to kits too — eqOnly() strips everything but the EQ.
        const bool focusIsKit = partIsKit (focus);
        partFxUse[(std::size_t) focus]  = focusIsKit ? eqOnly (liveFx) : liveFx;
        partLfoUse[(std::size_t) focus] = focusIsKit ? PartLfos{} : liveLfo;
        partMatrixUse[(std::size_t) focus] = focusIsKit ? ModMatrix{} : liveMatrixStore;
        for (int pt = 0; pt < maxParts; ++pt)
        {
            if (pt == focus) continue;                   // the live part is filled by renderParts
            const LockedPub& cur = lockedSlots[(std::size_t) pt].current();
            partParams[(std::size_t) pt] = cur.vp;
            if (partIsKit (pt)) { partFxUse[(std::size_t) pt] = eqOnly (cur.fx); partLfoUse[(std::size_t) pt] = PartLfos{}; }
            else                { partFxUse[(std::size_t) pt] = cur.fx;          partLfoUse[(std::size_t) pt] = cur.lfo; }
            partMatrixUse[(std::size_t) pt] = cur.mtx;   // baked per-part matrix (empty until a part bakes one)
        }
        if (! smoothPrimed)
        {
            smCutoff = liveParams.cutoffHz; smReso = liveParams.resonance;
            smL1 = liveParams.osc1Level; smL2 = liveParams.osc2Level; smL3 = liveParams.osc3Level;
            smoothPrimed = true;
        }
        // J1: per-LFO SYNC engage. Turning SYNC on does NOT jump the phase immediately — the LFO
        // keeps free-running until the NEXT bar boundary, then engages the transport-derived phase
        // (click-safe handoff). Turning it off resumes free-run from the current phase (continuous).
        const int barNow = (int) std::floor (transportBeats_ / 4.0);
        const bool barCrossed = barNow != lfoPrevBar_;
        for (int p = 0; p < maxParts; ++p)
            for (int k = 0; k < 3; ++k)
            {
                const bool want = partLfoUse[(std::size_t) p].lfo[k].synced;
                if (want && ! lfoPrevSynced[(std::size_t) p][(std::size_t) k])
                    lfoSyncEngaged[(std::size_t) p][(std::size_t) k] = false;   // just enabled -> pending until a bar
                if (! want)
                    lfoSyncEngaged[(std::size_t) p][(std::size_t) k] = false;   // disabled -> free-run
                if (want && barCrossed)
                    lfoSyncEngaged[(std::size_t) p][(std::size_t) k] = true;    // engage at the bar downbeat
                lfoPrevSynced[(std::size_t) p][(std::size_t) k] = want;
            }
        lfoPrevBar_ = barNow;

        partHadVoice = {};
        partLevelUse = { { 1.0f, 1.0f, 1.0f, 1.0f } };   // unity until setMix() (test path stays unity)
        partPanUse   = {};
        for (int p = 0; p < maxParts; ++p)
        {
            std::fill (partMono[(std::size_t) p].begin(),    partMono[(std::size_t) p].begin()    + numSamples, 0.0f);
            std::fill (partSampleL[(std::size_t) p].begin(), partSampleL[(std::size_t) p].begin() + numSamples, 0.0f);   // I2
            std::fill (partSampleR[(std::size_t) p].begin(), partSampleR[(std::size_t) p].begin() + numSamples, 0.0f);
        }
    }

    // Render active voices for [startSample, startSample+numSamples) into their parts'
    // buffers, smoothing part 0 and applying EACH PART's own three LFOs (partLfoUse) plus
    // the shared pitch-bend + mod-wheel vibrato. LFOs only advance for parts that have
    // active voices (a free-running LFO on a silent part costs nothing).
    void renderParts (int startSample, int numSamples, VoiceParams liveParams)
    {
        for (auto& v : voices) if (v.isActive()) partHadVoice[(std::size_t) v.getPart()] = true;
        for (auto& sv : sampleVoices) if (sv.isActive()) partHadVoice[(std::size_t) sv.getPart()] = true;   // I2

        int done = 0;
        while (done < numSamples)
        {
            const int chunk = std::min (kSmoothChunk, numSamples - done);

            smCutoff += smoothCoef * (liveParams.cutoffHz  - smCutoff);
            smReso   += smoothCoef * (liveParams.resonance - smReso);
            smL1     += smoothCoef * (liveParams.osc1Level - smL1);
            smL2     += smoothCoef * (liveParams.osc2Level - smL2);
            smL3     += smoothCoef * (liveParams.osc3Level - smL3);

            partParams[(std::size_t) liveIndex] = liveParams;
            partParams[(std::size_t) liveIndex].cutoffHz  = smCutoff; partParams[(std::size_t) liveIndex].resonance = smReso;
            partParams[(std::size_t) liveIndex].osc1Level = smL1; partParams[(std::size_t) liveIndex].osc2Level = smL2; partParams[(std::size_t) liveIndex].osc3Level = smL3;

            // Per-part LFO modulation this chunk (three LFOs, summed per destination). The
            // RAW bipolar LFO output is also captured for the mod matrix (a matrix slot's own
            // depth scales it, independent of the LFO's fixed-dest depth).
            std::array<float, maxParts> pPitch {}, pCut {}, pPw {};
            std::array<std::array<float, 3>, maxParts> lfoRaw {};
            for (int p = 0; p < maxParts; ++p)
            {
                if (! partHadVoice[(std::size_t) p]) continue;
                for (int k = 0; k < 3; ++k)
                {
                    const LfoConfig& c = partLfoUse[(std::size_t) p].lfo[k];
                    LFO& l = partLfo[(std::size_t) p][(std::size_t) k];
                    l.setShape (static_cast<LFO::Shape> (c.shape));
                    float raw;
                    if (c.synced && lfoSyncEngaged[(std::size_t) p][(std::size_t) k])
                    {                                                     // J1: transport-position-derived phase (bar-locked, continuous)
                        const double beats = transportBeats_ + (double) (startSample + done) / samplesPerBeat_;
                        raw = l.setPhase (beats / lfodiv::cycleBeats (c.division));
                    }
                    else                                                  // free-running Hz (also the pre-engage phase of a syncing LFO)
                    {
                        l.setRate (c.rate);
                        raw = l.advance (chunk);
                    }
                    lfoRaw[(std::size_t) p][(std::size_t) k] = raw;
                    const float v = raw * c.depth;
                    switch (c.dest)
                    {
                        case 1: pPitch[(std::size_t) p] += v * 2.0f;  break;
                        case 2: pCut  [(std::size_t) p] += v * 3.0f;  break;
                        case 3: pPw   [(std::size_t) p] += v * 0.45f; break;
                        default: break;
                    }
                }
            }
            // Publish the FOCUSED (live) part's current LFO mod per destination for the UI
            // knob animation (semitones / octaves / pw-units; 0 when that part is silent).
            focusMod[1].store (pPitch[(std::size_t) liveIndex], std::memory_order_relaxed);
            focusMod[2].store (pCut  [(std::size_t) liveIndex], std::memory_order_relaxed);
            focusMod[3].store (pPw   [(std::size_t) liveIndex], std::memory_order_relaxed);
            // Also publish the focused part's RAW LFO outputs (-1..1) so the processor can use
            // LFO 1-3 as block-tier mod sources (one-block latency, fine at control rate).
            for (int k = 0; k < 3; ++k)
                focusLfoRaw[(std::size_t) k].store (lfoRaw[(std::size_t) liveIndex][(std::size_t) k], std::memory_order_relaxed);

            // Per-part performance mods (bend + mod-wheel vibrato). The vibrato LFO advances
            // once per chunk (shared phase); its depth + the bend are per part.
            const float vibRaw = vibratoLFO.advance (chunk);

            // Per-part mod-matrix source snapshot (per-part fields only; the voice fills its
            // own velocity/env/note/random). Built only for parts whose matrix is live.
            std::array<ModSources, maxParts> partSrc {};
            for (int p = 0; p < maxParts; ++p)
            {
                if (! partMatrixUse[(std::size_t) p].active()) continue;
                ModSources& ps = partSrc[(std::size_t) p];
                ps.lfo[0] = lfoRaw[(std::size_t) p][0]; ps.lfo[1] = lfoRaw[(std::size_t) p][1]; ps.lfo[2] = lfoRaw[(std::size_t) p][2];
                ps.modWheel  = modWheel[(std::size_t) p];
                ps.pitchBend = pitchBendSemis[(std::size_t) p] / 12.0f;     // approximate normalized bend for the matrix
                for (int mi = 0; mi < 8; ++mi) ps.macro[(std::size_t) mi] = macroVals[(std::size_t) mi];
            }

            const int off = startSample + done;
            for (auto& v : voices)
            {
                if (! v.isActive()) continue;
                const int pt = v.getPart();
                const float bendVib = pitchBendSemis[(std::size_t) pt] + vibRaw * modWheel[(std::size_t) pt] * kVibratoSemis;
                VoiceParams p = paramsFor (pt, v.getSoundSlot());
                p.pitchModSemis += pPitch[(std::size_t) pt] + bendVib;
                p.cutoffModOct   = pCut[(std::size_t) pt];
                p.pwMod          = pPw[(std::size_t) pt];
                const bool mtxOn = partMatrixUse[(std::size_t) pt].active();
                v.render (partMono[(std::size_t) pt].data() + off, chunk, p,
                          mtxOn ? &partMatrixUse[(std::size_t) pt] : nullptr,
                          mtxOn ? &partSrc[(std::size_t) pt]       : nullptr);
            }
            // I2: sample-pad voices render (stereo) into their part's sample bus for THIS chunk.
            // No LFO/bend/matrix mods — a one-shot just plays; choke/steal handled on the voice.
            for (auto& sv : sampleVoices)
            {
                if (! sv.isActive()) continue;
                const int pt = sv.getPart();
                sv.render (partSampleL[(std::size_t) pt].data() + off,
                           partSampleR[(std::size_t) pt].data() + off, chunk);
            }
            done += chunk;
        }
    }

    // Number of parts actually processed (not skipped) in the last mixParts — the
    // silent-part-skip CPU control is observable here for tests.
    int partsProcessedLastBlock() const { return partsProcessed; }

    // Part mixer (Sub-phase 2): per-part output level + pan applied at the master sum.
    // Set each block from the APVTS (part 0..3). Defaults (1.0 / 0.0) = unity/centre.
    void setMix (const std::array<float, maxParts>& levels, const std::array<float, maxParts>& pans)
    {
        partLevelUse = levels; partPanUse = pans;
        if (! mixPrimed)        // snap the gain smoothers to the initial values (no startup ramp)
        { for (int p = 0; p < maxParts; ++p) { prevLg[(std::size_t) p] = targetLg (p); prevRg[(std::size_t) p] = targetRg (p); } mixPrimed = true; }
    }

    // Looper tap: which part's post-FX/post-pan contribution mixParts copies out for the
    // audio looper (-1 = none). captureL()/captureR() hold the last block's tap (zeros if
    // the part was silent/skipped or out of range).
    // Per-part looper capture: each part's post-FX/post-pan stereo contribution from the
    // last mixParts (zeros if that part was skipped). The 4-lane audio looper taps these.
    const float* capturePartL (int p) const { return (p >= 0 && p < maxParts) ? capPartL[(std::size_t) p].data() : nullptr; }
    const float* capturePartR (int p) const { return (p >= 0 && p < maxParts) ? capPartR[(std::size_t) p].data() : nullptr; }

    // Trim + per-part FX (partFxUse) + sum into the stereo master (once per block).
    void mixParts (float* L, float* R, int numSamples)
    {
        std::fill (L, L + numSamples, 0.0f);
        std::fill (R, R + numSamples, 0.0f);
        // Per-part looper capture taps: zeroed each block, each filled below with that part's
        // post-FX/post-pan contribution (a skipped part stays silent -> its lane records silence).
        for (std::size_t p = 0; p < (std::size_t) maxParts; ++p)
        {
            std::fill (capPartL[p].begin(), capPartL[p].begin() + numSamples, 0.0f);
            std::fill (capPartR[p].begin(), capPartR[p].begin() + numSamples, 0.0f);
        }
        partsProcessed = 0;

        for (int p = 0; p < maxParts; ++p)
        {
            float* m = partMono[(std::size_t) p].data();
            for (int i = 0; i < numSamples; ++i) m[i] *= voiceTrim;

            bool fxOn = false;
            for (int f = 0; f < FXChain::kNumFX; ++f) fxOn = fxOn || partFxUse[(std::size_t) p].enabled[f];

            if (partHadVoice[(std::size_t) p]) fxSilentBlocks[(std::size_t) p] = 0;
            const bool skip = ! partHadVoice[(std::size_t) p]
                              && (! fxOn || fxSilentBlocks[(std::size_t) p] >= kFxHoldBlocks);
            if (skip)
            {
                // Clear the chain's state ONCE on skip-entry (delay lines, reverb combs/
                // allpasses, chorus buffers + its mod LFO). Skip only engages after the
                // output has been silent, so there's nothing audible to lose — and the
                // chain resumes from a clean zero, so retrigger can't emit stale garbage.
                if (! fxCleared[(std::size_t) p]) { partFx[(std::size_t) p].reset(); fxCleared[(std::size_t) p] = true; }
                // Keep the gain smoothers at target while skipped, so resume never ramps.
                prevLg[(std::size_t) p] = targetLg (p); prevRg[(std::size_t) p] = targetRg (p);
                continue;
            }
            fxCleared[(std::size_t) p] = false;
            ++partsProcessed;

            float* sL = partL[(std::size_t) p].data();
            float* sR = partR[(std::size_t) p].data();
            const float* smpL = partSampleL[(std::size_t) p].data();   // I2: this part's stereo sample bus
            const float* smpR = partSampleR[(std::size_t) p].data();
            // Sample voices join the mono synth voices BEFORE the part FX (so they get the part's
            // chorus/delay/reverb/width + EQ + pan). Same voiceTrim as synth voices for consistent
            // loudness and bounded headroom.
            for (int i = 0; i < numSamples; ++i)
            {
                sL[i] = m[i] + smpL[i] * voiceTrim;
                sR[i] = m[i] + smpR[i] * voiceTrim;
            }
            if (fxOn) { partFx[(std::size_t) p].setParams (partFxUse[(std::size_t) p]); partFx[(std::size_t) p].process (sL, sR, numSamples); }

            // Mixer level/pan (0 dB-centre balance law), SMOOTHED: ramp the L/R gains from
            // last block's values to this block's target across the block, so a stepped
            // partN_level / pan (automation, MIDI-learn, a finger drag) never zippers.
            const float lgT = targetLg (p), rgT = targetRg (p);
            float lg = prevLg[(std::size_t) p], rg = prevRg[(std::size_t) p];
            const float dlg = (lgT - lg) / (float) numSamples, drg = (rgT - rg) / (float) numSamples;
            float* cpL = capPartL[(std::size_t) p].data();    // looper: tap THIS part's contribution
            float* cpR = capPartR[(std::size_t) p].data();
            for (int i = 0; i < numSamples; ++i)
            {
                lg += dlg; rg += drg;
                const float cl = sL[i] * lg, cr = sR[i] * rg;
                L[i] += cl; R[i] += cr;
                cpL[i] = cl; cpR[i] = cr;
            }
            prevLg[(std::size_t) p] = lgT; prevRg[(std::size_t) p] = rgT;

            if (! partHadVoice[(std::size_t) p])
            {
                float pk = 0.0f;
                for (int i = 0; i < numSamples; ++i) pk = std::max (pk, std::max (std::abs (sL[i]), std::abs (sR[i])));
                if (pk < 1.0e-5f) ++fxSilentBlocks[(std::size_t) p]; else fxSilentBlocks[(std::size_t) p] = 0;
            }
        }
    }

    // Whole-block convenience (tests): begin + render all + mix. The single-LFO args map
    // to part 0's LFO 1 (parts 1-3 get no LFO); the PartLfos overload is the full path.
    void renderMaster (float* L, float* R, int numSamples, VoiceParams liveParams,
                       float lfoRate, int lfoShape, float lfoDepth, int lfoDest,
                       const FXParams* partFxParams)
    {
        std::array<PartLfos, maxParts> pl {};
        pl[0].lfo[0] = { lfoRate, lfoDepth, lfoShape, lfoDest };
        renderMaster (L, R, numSamples, liveParams, pl.data(), partFxParams);
    }
    void renderMaster (float* L, float* R, int numSamples, VoiceParams liveParams,
                       const PartLfos* partLfos, const FXParams* partFxParams)
    {
        // Test path: the passed per-part arrays override every part (incl. locked/kit),
        // so a test can exercise any part's FX/LFO without going through the bake.
        beginMasterBlock (numSamples, liveParams, partFxParams[0], partLfos[0]);
        for (int p = 1; p < maxParts; ++p) { partFxUse[(std::size_t) p] = partFxParams[p]; partLfoUse[(std::size_t) p] = partLfos[p]; }
        renderParts (0, numSamples, liveParams);
        mixParts (L, R, numSamples);
    }

private:
    // ---- mono / legato (last-note priority) — PER PART ---------------------
    // Each part has its OWN mono voice + note stack, so a mono lead on part 1 glides and
    // holds independently of a kit hammering on part 4. The part's mono voice is picked
    // via the normal free-or-steal path (generators yield first), so a running sequencer
    // can never steal a live mono lead.
    void monoNoteOn (int note, float velocity, int part, int soundSlot, bool generator)
    {
        const std::size_t pt = (std::size_t) part;
        const bool hadNote = numMonoHeld[pt] > 0;
        pushHeld (part, note);

        int v = monoVoice[pt];
        const bool reuse = (v >= 0 && voices[(std::size_t) v].isActive() && voices[(std::size_t) v].getPart() == part);
        if (! reuse) { v = (int) pickVoice (part); monoVoice[pt] = v; }
        sustained[(std::size_t) v] = false;

        // Legato: glide to the new pitch without retriggering the amp env (only if the
        // part's own voice is already sounding). Mono / first note: retrigger.
        if (partPolyMode[pt] == 2 && hadNote && reuse)
            voices[(std::size_t) v].changeNote (note, ++eventCounter);
        else
        {
            const auto& pp = paramsFor (part, soundSlot);   // Tier 1a start-phase policy
            voices[(std::size_t) v].noteOn (note, velocity, ++eventCounter, part, soundSlot, generator,
                                            pp.osc1Phase, pp.osc2Phase, pp.osc3Phase);
        }
    }

    void monoNoteOff (int note, int part)
    {
        const std::size_t pt = (std::size_t) part;
        removeHeld (part, note);
        const int v = monoVoice[pt];
        if (v < 0 || ! voices[(std::size_t) v].isActive() || voices[(std::size_t) v].getPart() != part) return;
        if (numMonoHeld[pt] > 0)
            voices[(std::size_t) v].changeNote (monoHeld[pt][(std::size_t) (numMonoHeld[pt] - 1)], ++eventCounter);  // fall back
        else if (sustainPedal)
            sustained[(std::size_t) v] = true;
        else
            voices[(std::size_t) v].noteOff();
    }

    // Pick a voice for `part`: a free voice, else steal by the isolation priority
    // (oldest generator -> oldest own-part -> global oldest), same as the poly path.
    std::size_t pickVoice (int part)
    {
        for (std::size_t i = 0; i < activeVoiceLimit; ++i)
            if (! voices[i].isActive()) return i;
        int oldestGen = -1, oldestOwn = -1; std::size_t oldestAny = 0;
        for (std::size_t i = 0; i < activeVoiceLimit; ++i)
        {
            if (voices[i].isGenerator() && (oldestGen < 0 || voices[i].getTimestamp() < voices[(std::size_t) oldestGen].getTimestamp())) oldestGen = (int) i;
            if (voices[i].getPart() == part && (oldestOwn < 0 || voices[i].getTimestamp() < voices[(std::size_t) oldestOwn].getTimestamp())) oldestOwn = (int) i;
            if (voices[i].getTimestamp() < voices[oldestAny].getTimestamp()) oldestAny = i;
        }
        ++stealCounter;
        return (oldestGen >= 0) ? (std::size_t) oldestGen : (oldestOwn >= 0) ? (std::size_t) oldestOwn : oldestAny;
    }

    void pushHeld (int part, int note)
    {
        const std::size_t pt = (std::size_t) part;
        removeHeld (part, note);                    // move to top if already held
        if (numMonoHeld[pt] < 128) monoHeld[pt][(std::size_t) numMonoHeld[pt]++] = note;
    }
    void removeHeld (int part, int note)
    {
        const std::size_t pt = (std::size_t) part;
        for (int i = 0; i < numMonoHeld[pt]; ++i)
            if (monoHeld[pt][(std::size_t) i] == note)
            {
                for (int j = i; j < numMonoHeld[pt] - 1; ++j) monoHeld[pt][(std::size_t) j] = monoHeld[pt][(std::size_t)(j + 1)];
                --numMonoHeld[pt];
                return;
            }
    }

    static constexpr int kSmoothChunk = 16;   // sub-block size for param smoothing

    static constexpr float kVibratoSemis = 0.5f;   // max mod-wheel vibrato depth (+/-)

    std::array<SynthVoice, maxVoices> voices;
    static constexpr int kMaxSampleVoices = 16;    // I2: stereo sample-pad voices (kit one-shots)
    std::array<SampleVoice, kMaxSampleVoices> sampleVoices;
    std::array<bool, maxVoices> sustained {};      // key released but held by pedal
    std::size_t activeVoiceLimit = maxVoices;      // <= maxVoices; see setMaxVoices
    float voiceTrim = 0.25f;                        // 1/sqrt(maxVoices); headroom trim

    // Parts (7C): part 0 = LIVE (smoothed per chunk), parts 1-3 = LOCKED (baked,
    // published lock-free via a double buffer). A locked part publishes its voice params
    // + FX + LFOs together, so the audio thread reads a consistent snapshot with no race.
    struct LockedPub { VoiceParams vp; FXParams fx; PartLfos lfo; ModMatrix mtx; };
    struct LockedSlot
    {
        LockedPub buf[2];
        std::atomic<int> idx { 0 };
        const LockedPub& current() const { return buf[(std::size_t) idx.load (std::memory_order_acquire)]; }
        void publish (const LockedPub& p)             // message thread
        {
            const int w = 1 - idx.load (std::memory_order_relaxed);
            buf[(std::size_t) w] = p;
            idx.store (w, std::memory_order_release);
        }
    };
    std::array<VoiceParams, maxParts> partParams {};
    std::array<LockedSlot,  maxParts> lockedSlots {};
    std::array<FXParams, maxParts> partFxUse {};      // per-part FX in effect this block (part 0 live, 1-3 baked)
    std::array<PartLfos, maxParts> partLfoUse {};     // per-part LFOs in effect this block
    std::array<ModMatrix, maxParts> partMatrixUse {}; // per-part mod matrix in effect this block (#56)
    ModMatrix liveMatrixStore {};                     // the focused part's live matrix (set by the processor)
    std::array<float, 8> macroVals {};                // current macro knob values 0..1 (matrix sources)
    double transportBeats_ = 0.0, samplesPerBeat_ = 12000.0;   // J1: block-start beat position + samples/beat
    std::array<std::array<bool, 3>, maxParts> lfoSyncEngaged {}, lfoPrevSynced {};   // J1: per-LFO deferred sync engage
    int lfoPrevBar_ = -1000000;
    std::array<float, maxParts> partLevelUse { { 1.0f, 1.0f, 1.0f, 1.0f } };   // mixer level (unity default)
    std::array<float, maxParts> partPanUse   {};      // mixer pan (centre default = 0)
    std::array<float, maxParts> prevLg { { 1.0f, 1.0f, 1.0f, 1.0f } };         // per-block-ramped mixer L gain
    std::array<float, maxParts> prevRg { { 1.0f, 1.0f, 1.0f, 1.0f } };         // ...and R gain (anti-zipper)
    bool mixPrimed = false;                                                    // snap smoothers to first setMix
    float targetLg (int p) const { const float pan = partPanUse[(std::size_t) p]; return partLevelUse[(std::size_t) p] * (pan <= 0.0f ? 1.0f : 1.0f - pan); }
    float targetRg (int p) const { const float pan = partPanUse[(std::size_t) p]; return partLevelUse[(std::size_t) p] * (pan >= 0.0f ? 1.0f : 1.0f + pan); }

    // Kit parts: per-part double-buffered KitData (pads + baked params). Published on
    // the message thread; the audio thread reads buf[kitReadIdx[part]] (sampled once
    // per render). isKit=false (the default) means the part is a plain locked/live part.
    struct KitSlot
    {
        KitData buf[2];
        std::atomic<int> idx { 0 };
        const KitData& current() const { return buf[(std::size_t) idx.load (std::memory_order_acquire)]; }
        void publish (const KitData& k)               // message thread
        {
            const int w = 1 - idx.load (std::memory_order_relaxed);
            buf[(std::size_t) w] = k;
            idx.store (w, std::memory_order_release);
        }
    };
    std::array<KitSlot, maxParts> kitSlots;
    std::array<int, maxParts> kitReadIdx {};          // per-block snapshot of each slot's read buffer

    // Per-part FX (Sub-phase 2): one chain + mono/stereo scratch per part, all sized in
    // prepare(). fxSilentBlocks counts consecutive silent (voice-free, FX-tail-decayed)
    // blocks so an idle part's FX is skipped after a short hold.
    static constexpr int kFxHoldBlocks = 4;
    int maxBlockSize = 2048;
    std::array<FXChain, maxParts> partFx;
    std::array<std::vector<float>, maxParts> partMono, partL, partR;
    std::array<std::vector<float>, maxParts> partSampleL, partSampleR;   // I2: per-part stereo sample bus
    std::array<std::vector<float>, maxParts> capPartL, capPartR;   // per-part looper capture taps
    int liveKitPart = -1, liveKitPad = -1;   // kit pad routed through live panel params (edit mode)
    std::atomic<float> focusMod[4] { };   // focused part's LFO mod per dest (UI knob animation)
    std::atomic<float> focusLfoRaw[3] { };// focused part's raw LFO out (-1..1) for block-tier mod sources

public:
    // Current LFO modulation on the focused part for a destination (0 off, 1 pitch semis,
    // 2 cutoff octaves, 3 pw units). Audio thread writes; the UI reads for knob animation.
    float focusModForDest (int d) const
    { return (d >= 0 && d < 4) ? focusMod[(std::size_t) d].load (std::memory_order_relaxed) : 0.0f; }
    // Focused part's raw LFO output (-1..1) for block-tier mod sources (k = 0..2).
    float focusLfoRawOut (int k) const
    { return (k >= 0 && k < 3) ? focusLfoRaw[(std::size_t) k].load (std::memory_order_relaxed) : 0.0f; }
private:
    std::array<int, maxParts> fxSilentBlocks {};
    std::array<bool, maxParts> fxCleared {};          // FX state reset on skip-entry (no stale-resume pop)
    std::array<bool, maxParts> partHadVoice {};       // set across renderParts segments, read in mixParts
    int partsProcessed = 0;                           // parts not skipped in the last mixParts (test observability)

    // Kit note-off ledger (audio-thread POD): a trigger note -> the sounding notes it
    // fired, so a note-off releases exactly those (chord pads, decoupled pitch).
    struct KitLedgerEntry { int part = -1, trigger = -1, slot = 0, num = 0; int notes[kMaxPadSoundNotes] {}; };
    std::array<KitLedgerEntry, maxVoices * maxParts> kitLedger {};
    KitLedgerEntry* kitLedgerFor (int part, int trigger, bool createIfMissing)
    {
        for (auto& e : kitLedger) if (e.part == part && e.trigger == trigger) return &e;
        if (! createIfMissing) return nullptr;
        for (auto& e : kitLedger) if (e.part < 0) { e.part = part; e.trigger = trigger; return &e; }
        return nullptr;
    }

    LFO lfo, vibratoLFO;
    std::array<std::array<LFO, 3>, maxParts> partLfo;   // 3 per-part LFOs (Sub-phase 2)
    std::uint64_t eventCounter = 0;
    std::uint64_t stealCounter = 0;
    double sampleRate = 0.0;
    PolyBlepOscillator::Quality oscQuality = PolyBlepOscillator::Quality::Efficient;

    // Performance controllers.
    // Per-part performance controllers (G6): a surface routed to a part bends/vibratos ONLY
    // that part (a Launchkey bending its bass part must not warp the lead). Default 0 for
    // every part -> bit-identical to the old global behaviour when nothing is bending.
    std::array<float, maxParts> pitchBendSemis {}, modWheel {};
    bool  sustainPedal = false;

    // Mono/legato state — PER PART (each part has its own mode, note stack, and mono voice).
    std::array<int, maxParts> partPolyMode {};                  // per part: 0 poly, 1 mono, 2 legato
    std::array<std::array<int, 128>, maxParts> monoHeld {};     // per-part held-note stack (last = priority)
    std::array<int, maxParts> numMonoHeld {};
    std::array<int, maxParts> monoVoice { { -1, -1, -1, -1 } }; // per-part mono voice index (-1 = none)

    // Zipper smoothing state (global params).
    float smoothCoef = 0.05f, smCutoff = 0.0f, smReso = 0.0f;
    float smL1 = 0.8f, smL2 = 0.8f, smL3 = 0.0f;   // smoothed effective osc levels
    bool  smoothPrimed = false;
    int   liveIndex = 0;                            // the APVTS-driven (focused) part (1.3)
};
