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

    void setRecording (bool b) { recording_ = b; }
    void setPlaying   (bool b) { playing_ = b; }
    bool recording() const { return recording_; }
    bool playing()   const { return playing_; }

    void clear() { for (auto& p : parts) p.count = 0; loopPos = 0; }   // wipe events
    void reset() { clear(); recording_ = playing_ = false; }

    bool hasContent (int part) const { return part >= 0 && part < kParts && parts[(std::size_t) part].count > 0; }
    int  eventCount (int part) const { return (part >= 0 && part < kParts) ? parts[(std::size_t) part].count : 0; }
    const Event& event (int part, int i) const { return parts[(std::size_t) part].ev[(std::size_t) i]; }

    // Record an incoming performance note at (loop position + block offset). REC only.
    void recordNote (int part, int offsetInBlock, int note, float vel, bool on)
    {
        if (! recording_ || part < 0 || part >= kParts) return;
        auto& p = parts[(std::size_t) part];
        if (p.count >= kMaxEvents) return;
        const int t = (loopPos + (offsetInBlock < 0 ? 0 : offsetInBlock)) % loopLen;
        p.ev[(std::size_t) p.count++] = { t, note, vel, on, false };
    }

    // Emit this block's armed playback events. emit(part, note, vel, on). Block start.
    template <typename Emit>
    void playBlock (int numSamples, Emit&& emit) const
    {
        if (! playing_) return;
        const int start = loopPos, end = loopPos + numSamples;
        for (int part = 0; part < kParts; ++part)
        {
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
    bool recording_ = false, playing_ = false;
};
