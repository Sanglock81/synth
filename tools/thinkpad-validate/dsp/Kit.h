#pragma once
#include "SynthVoice.h"     // VoiceParams
#include <array>

// ============================================================================
// Kit part data (JUCE-free POD). A Kit part turns the paramsFor(part, note) seam
// per-note: each of up to 16 pads has its own baked VoiceParams (from a source
// preset, with its level folded into VoiceParams::gain), a trigger note, up to 4
// sounding notes (decoupling pitch from the trigger — 2..4 = a chord pad), and a
// choke group. All fixed-size; copied verbatim onto the audio thread.
// ============================================================================

static constexpr int kMaxKitPads      = 16;
static constexpr int kMaxPadSoundNotes = 4;

struct KitPad
{
    int  triggerNote = -1;                                   // -1 = empty pad (silent)
    int  soundNote[kMaxPadSoundNotes] = { 60, 0, 0, 0 };     // sounding pitches this pad plays
    int  numSound    = 1;                                    // 1 = single hit, 2..4 = chord pad
    int  chokeGroup  = 0;                                    // 0 = none; same nonzero group = mutually choking

    // I2: a pad can play a SAMPLE instead of the synth VoiceParams. The pointers are BORROWED
    // from the processor's SampleStore (immutable, never-freed) — this POD is copied verbatim
    // onto the audio thread, so it must carry only the borrowed pointers + metadata, never audio.
    bool         isSample   = false;
    const float* sampleL    = nullptr;
    const float* sampleR    = nullptr;
    int          sampleLen  = 0;
    double       sampleSR   = 48000.0;
    int          sampleRoot = 60;                            // note at which ratio == 1.0
    float        sampleGain = 1.0f;                          // pad level folded in (velocity applied at trigger)
};

struct KitData
{
    bool                              isKit = false;
    std::array<KitPad, kMaxKitPads>   pads {};
    std::array<VoiceParams, kMaxKitPads> params {};          // pad[i]'s baked voice params (gain = pad level)

    // Pad index whose trigger note == `note`, or -1 if none maps.
    int padForTrigger (int note) const
    {
        for (int i = 0; i < kMaxKitPads; ++i)
            if (pads[(std::size_t) i].triggerNote == note) return i;
        return -1;
    }
};
