#pragma once
#include "ADSREnvelope.h"
#include <cmath>
#include <cstdint>
#include <algorithm>

// ============================================================================
// One STEREO sample-playback voice for a kit pad (I2). JUCE-free, POD-driven,
// like SynthVoice: it holds no data of its own — the engine hands it BORROWED
// pointers into the message-side SampleStore (immutable, never-freed buffers).
//
//   * Pitch-tracked: reads at ratio = 2^((note-root)/12) * nativeSR/engineSR,
//     where the engineSR half is taken from prepare() (current device rate) so a
//     44.1<->48 device switch mid-session stays correct.
//   * 4-point cubic (Catmull-Rom) interpolation — linear grits up down-pitched
//     transients; a sampler deserves better than the chorus's linear delay read.
//   * Anti-click: a short fade in/out at the sample's ends AND a fast fade on
//     choke/steal (via ADSREnvelope::quickRelease) — the standing rule for any
//     path that can stop at non-zero amplitude.
//   * One-shot: note-off is ignored (drums play through); it ends at the buffer
//     end or when choked.
// ============================================================================

struct SamplePlay
{
    const float* L = nullptr;      // borrowed, immutable, outlives the voice
    const float* R = nullptr;
    int    len      = 0;
    double nativeSR = 48000.0;
    int    rootNote = 60;
    float  gain     = 1.0f;        // pad level * velocity, folded at note-on
};

class SampleVoice
{
public:
    void prepare (double newSampleRate)
    {
        engineSR = newSampleRate > 0.0 ? newSampleRate : 48000.0;
        env.prepare (engineSR);
        reset();
    }

    void reset() { active_ = false; env.reset(); cursor = 0.0; }

    void noteOn (const SamplePlay& s, int note, std::uint64_t stamp,
                 int partIndex, int slot, bool gen)
    {
        play      = s;
        midiNote  = note;
        timestamp = stamp;
        part      = partIndex;
        soundSlot = slot;
        generator = gen;
        cursor    = 0.0;
        ratio     = std::pow (2.0, (note - play.rootNote) / 12.0) * (play.nativeSR / engineSR);
        if (ratio < 1.0e-4) ratio = 1.0e-4;
        fadeSrc   = std::max (1.0, kFadeSeconds * play.nativeSR);     // fade length in SOURCE samples
        env.setParameters (0.0005, 0.0, 1.0, 0.010);                 // near-instant gate, full sustain
        env.noteOn();
        active_ = play.L != nullptr && play.R != nullptr && play.len > 1;
    }

    // Choke / steal: a ~4 ms fade to silence, then idle (click-free).
    void steal() { env.quickRelease(); }

    bool          isActive()   const { return active_; }
    int           getPart()    const { return part; }
    int           getSoundSlot() const { return soundSlot; }
    std::uint64_t stamp()      const { return timestamp; }
    bool          isGenerator() const { return generator; }

    // Add this voice's stereo output into outL/outR for n samples.
    void render (float* outL, float* outR, int n)
    {
        if (! active_) return;
        const double last = (double) (play.len - 1);
        for (int i = 0; i < n; ++i)
        {
            if (cursor >= last) { active_ = false; break; }

            float l, r; readCubic (cursor, l, r);

            // End/start anti-click ramps (in source-sample distance from each edge).
            const double fromStart = cursor;
            const double toEnd     = last - cursor;
            float edge = 1.0f;
            if (fromStart < fadeSrc) edge = std::min (edge, (float) (fromStart / fadeSrc));
            if (toEnd     < fadeSrc) edge = std::min (edge, (float) (toEnd     / fadeSrc));

            const float e = env.nextSample();            // gate + choke fade
            const float g = play.gain * e * edge;
            outL[i] += l * g;
            outR[i] += r * g;

            cursor += ratio;
            if (! env.isActive()) { active_ = false; break; }   // choke completed
        }
    }

private:
    void readCubic (double pos, float& l, float& r) const
    {
        const int i1 = (int) pos;
        const float f = (float) (pos - i1);
        const int i0 = i1 > 0 ? i1 - 1 : 0;
        const int i2 = i1 + 1 < play.len ? i1 + 1 : play.len - 1;
        const int i3 = i1 + 2 < play.len ? i1 + 2 : play.len - 1;
        l = catmull (play.L[i0], play.L[i1], play.L[i2], play.L[i3], f);
        r = catmull (play.R[i0], play.R[i1], play.R[i2], play.R[i3], f);
    }

    // 4-point Catmull-Rom spline through (y1..y2) with tangents from y0,y3.
    static float catmull (float y0, float y1, float y2, float y3, float t)
    {
        const float a0 = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
        const float a1 =         y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float a2 = -0.5f * y0             + 0.5f * y2;
        return ((a0 * t + a1) * t + a2) * t + y1;
    }

    static constexpr double kFadeSeconds = 0.003;   // 3 ms anti-click ramp at each end

    double       engineSR = 48000.0;
    SamplePlay   play;
    double       cursor = 0.0, ratio = 1.0, fadeSrc = 144.0;
    ADSREnvelope env;
    int          midiNote = 60, part = 0, soundSlot = 0;
    std::uint64_t timestamp = 0;
    bool         active_ = false, generator = false;
};
