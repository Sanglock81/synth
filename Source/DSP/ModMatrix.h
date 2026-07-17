#pragma once
#include <array>
#include <algorithm>
#include <cstdint>

// ============================================================================
// Mod matrix — JUCE-free, RT-safe (fixed storage, no alloc/lock/branch-on-string).
//
// A small routing table: kSlots independent slots, each { source -> dest, depth }.
// The engine fills a ModSources snapshot with the CURRENT value of every source
// (per part for LFO/wheel/macros, per voice for env/velocity/note/random), then a
// voice calls evaluate() once per control chunk to get the summed offset for each
// destination. Depth is bipolar (-1..1); at |depth|=1 and a full-scale source the
// destination moves by its documented range (kRange* below).
//
// Additive layer: this sits ON TOP of the fixed LFO dests, the env->pitch/cutoff
// routes and the velocity routes — it never replaces them. An all-inert matrix
// (every slot source/dest None, or depth 0) yields all-zero offsets, so a voice
// renders bit-identically (the golden guarantee). active() lets the caller skip
// the whole evaluation on the fast path.
// ============================================================================

// Current value of every modulation source, sampled at a control-chunk boundary.
// Per-part fields (lfo/modWheel/pitchBend/macro) are filled by the engine; per-voice
// fields (modEnv/ampEnv/velocity/noteNorm/random) by the voice.
struct ModSources
{
    float lfo[3]    { 0.0f, 0.0f, 0.0f };   // -1..1 raw LFO output
    float modEnv    = 0.0f;                 // 0..1 filter/mod envelope level
    float ampEnv    = 0.0f;                 // 0..1 amp envelope level
    float velocity  = 0.0f;                 // 0..1 note velocity
    float noteNorm  = 0.0f;                 // (note-60)/60, ~ -1..1 keytrack
    float modWheel  = 0.0f;                 // 0..1
    float pitchBend = 0.0f;                 // -1..1
    float random    = 0.0f;                 // -1..1 per-note sample & hold
    float macro[8]  { };                    // 0..1 each
};

class ModMatrix
{
public:
    static constexpr int kSlots = 8;

    // Persisted as ints — order is STABLE, only append.
    enum Source { SrcNone = 0, LFO1, LFO2, LFO3, ModEnv, AmpEnv, Velocity, Note,
                  ModWheel, PitchBend, Random, Macro1, Macro2, Macro3, Macro4,
                  Macro5, Macro6, Macro7, Macro8, kNumSources };
    // WavePos is RESERVED for the (not-yet-built) wavetable oscillator: it persists and
    // evaluates, but no voice seam consumes it yet, so a route to it is silent for now.
    //
    // VOICE-TIER dests (Pitch..Osc3Level) are applied per-voice inside SynthVoice via evaluate().
    // BLOCK-TIER dests (>= kFirstBlockDest) are applied by the PROCESSOR at block rate to the
    // focused part's FXParams/PartLfos/VoiceParams before they reach the engine — they modulate
    // params that aren't per-voice offsets (FX, EQ, LFO rate/depth, env stages, osc tune, ...).
    // Order is STABLE — only append. Registry metadata (paramId/name/category) lives in the
    // processor; the matrix only sums source*depth per dest id.
    enum Dest   { DstNone = 0, Pitch, Cutoff, Resonance, PulseWidth, Amp,
                  WavePos, Osc1Level, Osc2Level, Osc3Level,
                  // --- block-tier (append only) ---
                  ChorusRate, ChorusDepth, ChorusMix,
                  DelayTime, DelayFeedback, DelayMix, DelaySpread,
                  ReverbSize, ReverbDamp, ReverbWidth, ReverbMix,
                  StereoWidth,
                  EqB1Gain, EqB2Gain, EqB3Gain,
                  Lfo1Rate, Lfo1Depth, Lfo2Rate, Lfo2Depth, Lfo3Rate, Lfo3Depth,
                  AmpAttack, AmpDecay, AmpSustain, AmpRelease,
                  FltAttack, FltDecay, FltSustain, FltRelease,
                  FilterEnvAmt, FilterKeytrack, VelToCutoff, VelToAmp, FltEnvToPitch,
                  Osc1Octave, Osc1Detune, Osc2Octave, Osc2Detune, Osc3Octave, Osc3Detune,
                  GlideTime, PartLevel, PartPan,
                  kNumDests };
    static constexpr int kFirstBlockDest = ChorusRate;
    static constexpr int kNumBlockDests  = kNumDests - kFirstBlockDest;

    struct Slot { int source = SrcNone; int dest = DstNone; float depth = 0.0f; };  // depth -1..1

    // Summed per-destination offsets from all live slots.
    struct Offsets
    {
        float pitchSemis = 0.0f;   // added to the pitch exponent (semitones)
        float cutoffOct  = 0.0f;   // added to the cutoff exponent (octaves)
        float reso       = 0.0f;   // added to resonance (0..1 domain)
        float pw         = 0.0f;   // added to pulse width
        float amp        = 0.0f;   // amp multiplier is clamp(1 + amp, 0, 2)
        float wavePos    = 0.0f;   // RESERVED (wavetable position) — no seam consumes it yet
        float osc1Level  = 0.0f;   // added to osc 1/2/3 level (0..1 domain)
        float osc2Level  = 0.0f;
        float osc3Level  = 0.0f;
    };

    // Full-scale destination ranges at |depth| = 1 and a full-scale source.
    static constexpr float kRangePitch    = 24.0f;   // +/- 2 octaves
    static constexpr float kRangeCutoff   = 4.0f;    // +/- 4 octaves
    static constexpr float kRangeReso     = 1.0f;    // +/- full resonance
    static constexpr float kRangePw       = 0.45f;   // matches the LFO->PW range
    static constexpr float kRangeAmp      = 1.0f;    // +/- full (mul 0..2)
    static constexpr float kRangeWavePos  = 1.0f;    // +/- full wavetable sweep (reserved)
    static constexpr float kRangeOscLevel = 1.0f;    // +/- full source level

    std::array<Slot, kSlots> slots { };

    // Any live routing? Lets the voice/engine skip evaluation entirely (bit-identical).
    bool active() const
    {
        for (auto& s : slots)
            if (s.source != SrcNone && s.dest != DstNone && s.depth != 0.0f) return true;
        return false;
    }

    // Block-tier: sum the NORMALIZED (0..1 param-space) offset for each block dest into
    // out[dest - kFirstBlockDest]. The processor adds these to the focused part's param values.
    // out must have kNumBlockDests entries. Voice-tier dests are ignored here.
    void blockOffsets (const ModSources& s, float* out, int n) const
    {
        for (int i = 0; i < n; ++i) out[i] = 0.0f;
        for (auto& sl : slots)
        {
            if (sl.source == SrcNone || sl.dest < kFirstBlockDest || sl.depth == 0.0f) continue;
            const int idx = sl.dest - kFirstBlockDest;
            if (idx >= 0 && idx < n) out[idx] += sourceValue (sl.source, s) * sl.depth;   // -1..1 normalized
        }
    }

    Offsets evaluate (const ModSources& s) const
    {
        Offsets o;
        for (auto& sl : slots)
        {
            if (sl.source == SrcNone || sl.dest == DstNone || sl.depth == 0.0f) continue;
            const float v = sourceValue (sl.source, s) * sl.depth;
            switch (sl.dest)
            {
                case Pitch:      o.pitchSemis += v * kRangePitch;    break;
                case Cutoff:     o.cutoffOct  += v * kRangeCutoff;   break;
                case Resonance:  o.reso       += v * kRangeReso;     break;
                case PulseWidth: o.pw         += v * kRangePw;       break;
                case Amp:        o.amp        += v * kRangeAmp;      break;
                case WavePos:    o.wavePos    += v * kRangeWavePos;  break;   // reserved
                case Osc1Level:  o.osc1Level  += v * kRangeOscLevel; break;
                case Osc2Level:  o.osc2Level  += v * kRangeOscLevel; break;
                case Osc3Level:  o.osc3Level  += v * kRangeOscLevel; break;
                default: break;
            }
        }
        return o;
    }

private:
    static float sourceValue (int src, const ModSources& s)
    {
        switch (src)
        {
            case LFO1:      return s.lfo[0];
            case LFO2:      return s.lfo[1];
            case LFO3:      return s.lfo[2];
            case ModEnv:    return s.modEnv;
            case AmpEnv:    return s.ampEnv;
            case Velocity:  return s.velocity;
            case Note:      return s.noteNorm;
            case ModWheel:  return s.modWheel;
            case PitchBend: return s.pitchBend;
            case Random:    return s.random;
            case Macro1: case Macro2: case Macro3: case Macro4:
            case Macro5: case Macro6: case Macro7: case Macro8:
                return s.macro[(std::size_t) (src - Macro1)];
            default: return 0.0f;
        }
    }
};
