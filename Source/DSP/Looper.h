#pragma once
#include <array>

// ============================================================================
// Per-part MIDI looper — JUCE-free, RT-safe (fixed storage, no alloc/lock).
//
// A tape-style loop of MIDI note events per part. The owner (the processor) plays
// back this block's events at block start, records incoming performance notes into
// the loop while REC is armed, then advances the loop clock by the block length.
//
// First-pass double-trigger is avoided by ARMING: a freshly recorded event is not
// played until the loop next wraps (the live note already sounded this pass); on
// each wrap every event in that lane is armed, so overdubs join on the following cycle.
// Playback timing is block-granular (consistent with the synth's routed-note path);
// sample-accurate loop playback is a future refinement.
//
// J2 — PER-LANE LENGTH. Each lane (== part) has its OWN loop length. A single master
// position advances the clock; a lane's effective position is masterPos % laneLen.
// Because every length is a power-of-two multiple of a bar (1..32) and the master
// wraps at the longest possible length (32 bars, a common multiple of all of them),
// every lane stays phase-aligned to the same downbeat with no drift and no reset
// pulses — a 2-bar lane simply wraps 4x inside an 8-bar lane, always on the beat.
// ============================================================================

class Looper
{
public:
    static constexpr int kParts     = 4;
    static constexpr int kMaxEvents = 512;   // per part

    struct Event { int t; int note; float vel; bool on; bool armed; };
    struct Lane { std::array<Event, kMaxEvents> ev { }; int count = 0; };   // one lane's recorded clip

    // J3 scenes: snapshot / restore a lane's recorded clip (a plain copy of its events).
    const Lane& laneContent (int part) const
    { return parts[(std::size_t) (part < 0 ? 0 : part >= kParts ? kParts - 1 : part)]; }
    void setLaneContent (int part, const Lane& l) { if (part >= 0 && part < kParts) parts[(std::size_t) part] = l; }

    // The master wrap period — a common multiple of every lane length so masterPos % laneLen
    // is continuous across the wrap. Set from tempo each block (32 bars); clamped >= 1.
    void setMasterLength (int samples)
    {
        masterLen = samples < 1 ? 1 : samples;
        if (loopPos >= masterLen) loopPos %= masterLen;
    }

    // Per-lane length (samples). Each lane divides masterLen (32 / {1,2,4,8,16,32} bars).
    void setLoopLength (int part, int samples)
    {
        if (part < 0 || part >= kParts) return;
        loopLen[(std::size_t) part] = samples < 1 ? 1 : samples;
    }
    int  loopLength (int part) const { return (part >= 0 && part < kParts) ? loopLen[(std::size_t) part] : 1; }

    int  position() const            { return loopPos; }                       // master (arp/seq bar anchor)
    int  position (int part) const                                             // this lane's phase
    { return (part >= 0 && part < kParts) ? loopPos % loopLen[(std::size_t) part] : 0; }

    // Per-lane transport: lane N (== part N) has its OWN REC/PLAY, so each part records and
    // plays independently and NOTHING here depends on the edit/play focus (task #47).
    void setRecording (int part, bool b) { if (part >= 0 && part < kParts) recording_[(std::size_t) part] = b; }
    void setPlaying   (int part, bool b) { if (part >= 0 && part < kParts) playing_[(std::size_t) part]   = b; }
    bool recording (int part) const { return part >= 0 && part < kParts && recording_[(std::size_t) part]; }
    bool playing   (int part) const { return part >= 0 && part < kParts && playing_[(std::size_t) part]; }
    bool anyPlaying() const { for (bool b : playing_) if (b) return true; return false; }

    void clear (int part) { if (part >= 0 && part < kParts) parts[(std::size_t) part].count = 0; }   // wipe ONE lane
    void clearAll() { for (auto& p : parts) p.count = 0; loopPos = 0; }
    void reset() { clearAll(); for (int i = 0; i < kParts; ++i) { recording_[(std::size_t) i] = false; playing_[(std::size_t) i] = false; } }

    bool hasContent (int part) const { return part >= 0 && part < kParts && parts[(std::size_t) part].count > 0; }
    int  eventCount (int part) const { return (part >= 0 && part < kParts) ? parts[(std::size_t) part].count : 0; }
    const Event& event (int part, int i) const { return parts[(std::size_t) part].ev[(std::size_t) i]; }

    // Quantize (task #53): snap recorded note timestamps to a grid (1/32 = samplesPerStep/2).
    // Per-lane on/off; the grid is shared (from tempo). Default on.
    void setQuantizeGrid (int gridSamples) { quantGrid = gridSamples > 0 ? gridSamples : 0; }
    void setQuantize (int part, bool on) { if (part >= 0 && part < kParts) quantize_[(std::size_t) part] = on; }
    bool quantize (int part) const { return part >= 0 && part < kParts && quantize_[(std::size_t) part]; }

    // Record an incoming performance note at (lane position + block offset). Lane N records
    // only while ITS OWN REC is armed (part is the note's routed part — fixed, not focus).
    void recordNote (int part, int offsetInBlock, int note, float vel, bool on)
    {
        if (part < 0 || part >= kParts || ! recording_[(std::size_t) part]) return;
        auto& p = parts[(std::size_t) part];
        if (p.count >= kMaxEvents) return;
        const int len = loopLen[(std::size_t) part];
        int t = (position (part) + (offsetInBlock < 0 ? 0 : offsetInBlock)) % len;
        if (quantize_[(std::size_t) part] && quantGrid > 0 && note >= 0 && note < 128)
        {
            const int g = quantGrid;
            t = (((t + g / 2) / g) * g) % len;                          // snap to the nearest grid line
            if (on) lastOnT[(std::size_t) part][(std::size_t) note] = t;
            else if (lastOnT[(std::size_t) part][(std::size_t) note] == t)
                t = (t + g) % len;                                      // note-off snapped onto its on -> +1 grid (no zero-length)
        }
        p.ev[(std::size_t) p.count++] = { t, note, vel, on, false };
    }

    // Emit this block's armed playback events for every PLAYING lane, each against its OWN length.
    // emit(part, note, vel, on).
    template <typename Emit>
    void playBlock (int numSamples, Emit&& emit) const
    {
        for (int part = 0; part < kParts; ++part)
        {
            if (! playing_[(std::size_t) part]) continue;   // this lane is stopped
            const int len   = loopLen[(std::size_t) part];
            const int start = loopPos % len, end = start + numSamples;
            const auto& p = parts[(std::size_t) part];
            for (int i = 0; i < p.count; ++i)
            {
                const auto& e = p.ev[(std::size_t) i];
                if (! e.armed) continue;
                const bool inWin = (end <= len) ? (e.t >= start && e.t < end)
                                                : (e.t >= start || e.t < end - len);
                if (inWin) emit (part, e.note, e.vel, e.on);
            }
        }
    }

    // Advance the master clock; on a lane's OWN wrap, arm that lane's events for the next cycle.
    void advance (int numSamples)
    {
        for (int part = 0; part < kParts; ++part)
        {
            const int len = loopLen[(std::size_t) part];
            if ((loopPos % len) + numSamples >= len) armLane (part);    // this lane wrapped
        }
        loopPos += numSamples;
        if (loopPos >= masterLen) loopPos %= masterLen;
    }

private:
    void armLane (int part)
    {
        auto& p = parts[(std::size_t) part];
        for (int i = 0; i < p.count; ++i) p.ev[(std::size_t) i].armed = true;
    }

    std::array<Lane, kParts> parts { };
    std::array<int, kParts> loopLen { { 48000, 48000, 48000, 48000 } };  // per-lane length (samples)
    int  masterLen = 48000;                                              // master wrap period (32 bars)
    int  loopPos = 0;                                                    // master position [0, masterLen)
    std::array<bool, kParts> recording_ { };
    std::array<bool, kParts> playing_ { };
    std::array<bool, kParts> quantize_ { { true, true, true, true } };   // per-lane 1/32 quantize (default on)
    int quantGrid = 0;                                                   // grid in samples (0 = off; set from tempo)
    std::array<std::array<int, 128>, kParts> lastOnT {};                 // snapped pos of last note-on (pairing guard)
};
