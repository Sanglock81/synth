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
// each wrap every event is armed, so overdubs join on the following cycle.
// Playback timing is block-granular (consistent with the synth's routed-note path);
// sample-accurate loop playback is a future refinement.
// ============================================================================

class Looper
{
public:
    static constexpr int kParts     = 4;
    static constexpr int kMaxEvents = 512;   // per part

    struct Event { int t; int note; float vel; bool on; bool armed; };

    void setLoopLength (int samples)
    {
        loopLen = samples < 1 ? 1 : samples;
        if (loopPos >= loopLen) loopPos %= loopLen;
    }
    int  loopLength() const { return loopLen; }
    int  position() const   { return loopPos; }

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

    // Record an incoming performance note at (loop position + block offset). Lane N records
    // only while ITS OWN REC is armed (part is the note's routed part — fixed, not focus).
    void recordNote (int part, int offsetInBlock, int note, float vel, bool on)
    {
        if (part < 0 || part >= kParts || ! recording_[(std::size_t) part]) return;
        auto& p = parts[(std::size_t) part];
        if (p.count >= kMaxEvents) return;
        int t = (loopPos + (offsetInBlock < 0 ? 0 : offsetInBlock)) % loopLen;
        if (quantize_[(std::size_t) part] && quantGrid > 0 && note >= 0 && note < 128)
        {
            const int g = quantGrid;
            t = (((t + g / 2) / g) * g) % loopLen;                      // snap to the nearest grid line
            if (on) lastOnT[(std::size_t) part][(std::size_t) note] = t;
            else if (lastOnT[(std::size_t) part][(std::size_t) note] == t)
                t = (t + g) % loopLen;                                  // note-off snapped onto its on -> +1 grid (no zero-length)
        }
        p.ev[(std::size_t) p.count++] = { t, note, vel, on, false };
    }

    // Emit this block's armed playback events for every PLAYING lane. emit(part, note, vel, on).
    template <typename Emit>
    void playBlock (int numSamples, Emit&& emit) const
    {
        const int start = loopPos, end = loopPos + numSamples;
        for (int part = 0; part < kParts; ++part)
        {
            if (! playing_[(std::size_t) part]) continue;   // this lane is stopped
            const auto& p = parts[(std::size_t) part];
            for (int i = 0; i < p.count; ++i)
            {
                const auto& e = p.ev[(std::size_t) i];
                if (! e.armed) continue;
                const bool inWin = (end <= loopLen) ? (e.t >= start && e.t < end)
                                                    : (e.t >= start || e.t < end - loopLen);
                if (inWin) emit (part, e.note, e.vel, e.on);
            }
        }
    }

    // Advance the loop clock; on wrap, arm every event so it plays next cycle.
    void advance (int numSamples)
    {
        loopPos += numSamples;
        if (loopPos >= loopLen) { loopPos %= loopLen; armAll(); }
    }

private:
    void armAll() { for (auto& p : parts) for (int i = 0; i < p.count; ++i) p.ev[(std::size_t) i].armed = true; }

    struct Lane { std::array<Event, kMaxEvents> ev { }; int count = 0; };
    std::array<Lane, kParts> parts { };
    int  loopLen = 48000;
    int  loopPos = 0;
    std::array<bool, kParts> recording_ { };
    std::array<bool, kParts> playing_ { };
    std::array<bool, kParts> quantize_ { { true, true, true, true } };   // per-lane 1/32 quantize (default on)
    int quantGrid = 0;                                                   // grid in samples (0 = off; set from tempo)
    std::array<std::array<int, 128>, kParts> lastOnT {};                 // snapped pos of last note-on (pairing guard)
};
