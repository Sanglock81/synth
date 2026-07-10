#pragma once
#include "SynthVoice.h"
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
struct LfoConfig { float rate = 2.0f, depth = 0.0f; int shape = 0, dest = 0; };
struct PartLfos  { LfoConfig lfo[3]; };

class SynthEngine
{
public:
    static constexpr int maxVoices = 16;

    void prepare (double newSampleRate, int maxBlock = 2048)
    {
        sampleRate = newSampleRate;
        maxBlockSize = maxBlock;
        for (auto& v : voices)
        {
            v.setOscQuality (oscQuality);
            v.prepare (sampleRate);
        }
        // Per-part FX (Sub-phase 2): each part owns a chain + stereo scratch, all sized
        // now so renderMaster() never allocates. Skipped for silent/bypassed parts.
        for (int p = 0; p < maxParts; ++p)
        {
            partFx[(std::size_t) p].prepare (sampleRate, maxBlock);
            partMono[(std::size_t) p].assign ((std::size_t) maxBlock, 0.0f);
            partL[(std::size_t) p].assign ((std::size_t) maxBlock, 0.0f);
            partR[(std::size_t) p].assign ((std::size_t) maxBlock, 0.0f);
            fxSilentBlocks[(std::size_t) p] = kFxHoldBlocks;   // start idle
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
    void noteOn (int note, float velocity, int part = 0, int soundSlot = 0)
    {
        if (polyMode != 0) { monoNoteOn (note, velocity, part, soundSlot); return; }

        // Reuse a voice already playing this (note, part) — retrigger.
        for (std::size_t i = 0; i < activeVoiceLimit; ++i)
            if (voices[i].isActive() && voices[i].getNote() == note && voices[i].getPart() == part)
                { sustained[i] = false; voices[i].noteOn (note, velocity, ++eventCounter, part, soundSlot); return; }

        // Otherwise find a free voice...
        for (std::size_t i = 0; i < activeVoiceLimit; ++i)
            if (! voices[i].isActive())
                { sustained[i] = false; voices[i].noteOn (note, velocity, ++eventCounter, part, soundSlot); return; }

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
        voices[oldest].noteOn (note, velocity, ++eventCounter, part, soundSlot);
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
        for (auto& e : kitLedger) { e.part = -1; e.trigger = -1; e.num = 0; }
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

    // Publish a locked part's baked voice params + its FX + its 3 LFOs (message thread;
    // lock-free double-buffer, audio thread reads the current slot each block). The FX
    // and LFOs travel WITH the voice params so a locked part is fully self-contained and
    // there is no per-part FX/LFO data race with the audio thread (Sub-phase 2).
    void setLockedPartParams (int part, const VoiceParams& vp,
                              const FXParams& fx = {}, const PartLfos& lfo = {})
    {
        if (part >= 1 && part < maxParts) lockedSlots[(std::size_t) part].publish ({ vp, fx, lfo });
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
    void kitNoteOn (int part, int trigger, float velocity)
    {
        if (part < 1 || part >= maxParts) return;
        const KitData& k = kitSlots[(std::size_t) part].current();
        const int pad = k.padForTrigger (trigger);
        if (pad < 0) return;                                    // no pad here -> silence

        const int group = k.pads[(std::size_t) pad].chokeGroup;
        if (group != 0)                                         // cross-pad choke: quick-release the group
            for (auto& v : voices)
                if (v.isActive() && v.getPart() == part && v.getSoundSlot() != pad
                    && k.pads[(std::size_t) v.getSoundSlot()].chokeGroup == group)
                    v.steal();

        auto* e = kitLedgerFor (part, trigger, true);
        if (e != nullptr) { e->num = 0; e->slot = pad; }
        const auto& pd = k.pads[(std::size_t) pad];
        for (int i = 0; i < pd.numSound && i < kMaxPadSoundNotes; ++i)
        {
            noteOn (pd.soundNote[(std::size_t) i], velocity, part, pad);   // self-choke = retrigger same voice
            if (e != nullptr && e->num < kMaxPadSoundNotes) e->notes[(std::size_t) e->num++] = pd.soundNote[(std::size_t) i];
        }
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
        const auto& kb = kitSlots[(std::size_t) p].buf[(std::size_t) kitReadIdx[(std::size_t) p]];
        if (kb.isKit) return kb.params[(std::size_t) ((slot >= 0 && slot < kMaxKitPads) ? slot : 0)];
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
            const float vib = vibratoLFO.advance (chunk) * modWheel * kVibratoSemis;
            modPitch += pitchBendSemis + vib;

            // Each active voice renders with ITS part's params + the shared mods.
            for (auto& v : voices)
            {
                if (! v.isActive()) continue;
                VoiceParams p = paramsFor (v.getPart(), v.getSoundSlot());
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

    // ---- multitimbral master render (Sub-phase 2) --------------------------
    // Full STEREO master: each part's voices render into that part's own buffer, run
    // through that part's own FX chain, then sum (unity — per-part level/pan is a
    // deferred mixer). A part with no active voices AND an idle FX chain is SKIPPED
    // (the CPU control; a decaying FX tail keeps processing until silent). part 0 is
    // the smoothed LIVE part; the shared LFO/bend/vibrato drive every part (per-part
    // LFOs land next). Split into begin/renderParts/mixParts so the processor can keep
    // host MIDI sample-accurate (render voices per event segment) while FX+sum run once
    // per block. renderMaster() is the whole-block convenience for tests.

    // Clear per-part accumulators + snapshot locked/kit state for the block. part 0 uses
    // the caller's LIVE FX + LFOs; parts 1-3 use the FX/LFOs published WITH their baked
    // voice params (a kit part is dry). partFxUse/partLfoUse become the per-part config
    // for renderParts/mixParts — no data race with the message thread.
    void beginMasterBlock (int numSamples, VoiceParams liveParams,
                           const FXParams& live0Fx, const PartLfos& live0Lfo)
    {
        for (int pt = 0; pt < maxParts; ++pt)
            kitReadIdx[(std::size_t) pt] = kitSlots[(std::size_t) pt].idx.load (std::memory_order_acquire);

        partFxUse[0]  = live0Fx;
        partLfoUse[0] = live0Lfo;
        for (int pt = 1; pt < maxParts; ++pt)
        {
            const LockedPub& cur = lockedSlots[(std::size_t) pt].current();
            partParams[(std::size_t) pt] = cur.vp;
            if (partIsKit (pt)) { partFxUse[(std::size_t) pt] = FXParams{}; partLfoUse[(std::size_t) pt] = PartLfos{}; }
            else                { partFxUse[(std::size_t) pt] = cur.fx;     partLfoUse[(std::size_t) pt] = cur.lfo; }
        }
        if (! smoothPrimed)
        {
            smCutoff = liveParams.cutoffHz; smReso = liveParams.resonance;
            smL1 = liveParams.osc1Level; smL2 = liveParams.osc2Level; smL3 = liveParams.osc3Level;
            smoothPrimed = true;
        }
        partHadVoice = {};
        partLevelUse = { { 1.0f, 1.0f, 1.0f, 1.0f } };   // unity until setMix() (test path stays unity)
        partPanUse   = {};
        for (int p = 0; p < maxParts; ++p)
            std::fill (partMono[(std::size_t) p].begin(), partMono[(std::size_t) p].begin() + numSamples, 0.0f);
    }

    // Render active voices for [startSample, startSample+numSamples) into their parts'
    // buffers, smoothing part 0 and applying EACH PART's own three LFOs (partLfoUse) plus
    // the shared pitch-bend + mod-wheel vibrato. LFOs only advance for parts that have
    // active voices (a free-running LFO on a silent part costs nothing).
    void renderParts (int startSample, int numSamples, VoiceParams liveParams)
    {
        for (auto& v : voices) if (v.isActive()) partHadVoice[(std::size_t) v.getPart()] = true;

        int done = 0;
        while (done < numSamples)
        {
            const int chunk = std::min (kSmoothChunk, numSamples - done);

            smCutoff += smoothCoef * (liveParams.cutoffHz  - smCutoff);
            smReso   += smoothCoef * (liveParams.resonance - smReso);
            smL1     += smoothCoef * (liveParams.osc1Level - smL1);
            smL2     += smoothCoef * (liveParams.osc2Level - smL2);
            smL3     += smoothCoef * (liveParams.osc3Level - smL3);

            partParams[0] = liveParams;
            partParams[0].cutoffHz  = smCutoff; partParams[0].resonance = smReso;
            partParams[0].osc1Level = smL1; partParams[0].osc2Level = smL2; partParams[0].osc3Level = smL3;

            // Per-part LFO modulation this chunk (three LFOs, summed per destination).
            std::array<float, maxParts> pPitch {}, pCut {}, pPw {};
            for (int p = 0; p < maxParts; ++p)
            {
                if (! partHadVoice[(std::size_t) p]) continue;
                for (int k = 0; k < 3; ++k)
                {
                    const LfoConfig& c = partLfoUse[(std::size_t) p].lfo[k];
                    LFO& l = partLfo[(std::size_t) p][(std::size_t) k];
                    l.setRate (c.rate); l.setShape (static_cast<LFO::Shape> (c.shape));
                    const float v = l.advance (chunk) * c.depth;
                    switch (c.dest)
                    {
                        case 1: pPitch[(std::size_t) p] += v * 2.0f;  break;
                        case 2: pCut  [(std::size_t) p] += v * 3.0f;  break;
                        case 3: pPw   [(std::size_t) p] += v * 0.45f; break;
                        default: break;
                    }
                }
            }
            // Shared performance mods (bend + mod-wheel vibrato) hit every part's pitch.
            const float vib = vibratoLFO.advance (chunk) * modWheel * kVibratoSemis;
            const float bendVib = pitchBendSemis + vib;

            const int off = startSample + done;
            for (auto& v : voices)
            {
                if (! v.isActive()) continue;
                const int pt = v.getPart();
                VoiceParams p = paramsFor (pt, v.getSoundSlot());
                p.pitchModSemis += pPitch[(std::size_t) pt] + bendVib;
                p.cutoffModOct   = pCut[(std::size_t) pt];
                p.pwMod          = pPw[(std::size_t) pt];
                v.render (partMono[(std::size_t) pt].data() + off, chunk, p);
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

    // Trim + per-part FX (partFxUse) + sum into the stereo master (once per block).
    void mixParts (float* L, float* R, int numSamples)
    {
        std::fill (L, L + numSamples, 0.0f);
        std::fill (R, R + numSamples, 0.0f);
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
            for (int i = 0; i < numSamples; ++i) { sL[i] = m[i]; sR[i] = m[i]; }
            if (fxOn) { partFx[(std::size_t) p].setParams (partFxUse[(std::size_t) p]); partFx[(std::size_t) p].process (sL, sR, numSamples); }

            // Mixer level/pan (0 dB-centre balance law), SMOOTHED: ramp the L/R gains from
            // last block's values to this block's target across the block, so a stepped
            // partN_level / pan (automation, MIDI-learn, a finger drag) never zippers.
            const float lgT = targetLg (p), rgT = targetRg (p);
            float lg = prevLg[(std::size_t) p], rg = prevRg[(std::size_t) p];
            const float dlg = (lgT - lg) / (float) numSamples, drg = (rgT - rg) / (float) numSamples;
            for (int i = 0; i < numSamples; ++i) { lg += dlg; rg += drg; L[i] += sL[i] * lg; R[i] += sR[i] * rg; }
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
    // ---- mono / legato (last-note priority, voice 0) -----------------------
    // Mono/legato is single-timbre in v1 (voice 0). `part` is carried through so a
    // routed surface in mono still selects its params; multitimbral play uses poly.
    void monoNoteOn (int note, float velocity, int part = 0, int soundSlot = 0)
    {
        const bool hadNote = numHeld > 0;
        pushHeld (note);
        sustained[0] = false;

        // Legato: while a note is already sounding, glide to the new pitch
        // without retriggering the envelope. Mono (and the first note): retrigger.
        if (polyMode == 2 && hadNote && voices[0].isActive())
            voices[0].changeNote (note, ++eventCounter);
        else
            voices[0].noteOn (note, velocity, ++eventCounter, part, soundSlot);
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
    // published lock-free via a double buffer). A locked part publishes its voice params
    // + FX + LFOs together, so the audio thread reads a consistent snapshot with no race.
    struct LockedPub { VoiceParams vp; FXParams fx; PartLfos lfo; };
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
