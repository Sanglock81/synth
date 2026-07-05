#pragma once
#include <juce_core/juce_core.h>
#include "RtLogRing.h"
#include <atomic>
#include <vector>
#include <functional>
#include <memory>

// ============================================================================
// Audio-health telemetry + RT-safe logging front end.
//
// The audio thread calls the logXxx() methods, which only push POD events into
// the lock-free ring (no alloc / lock / IO). A low-priority background thread
// drains the ring, aggregates render times into min/median/p99/max, logs a
// stats line every ~10 s, logs overruns immediately, and tracks voice high-water
// and steal counts. A message-thread-readable snapshot feeds the debug overlay.
//
// Testability: the log sink is injectable (default = juce::FileLogger) and the
// drain/stats steps can be pumped synchronously (drainNow / flushStats), so the
// formatting is unit-tested without depending on the background thread's timing.
// ============================================================================

class AudioHealthLogger : private juce::Thread
{
public:
    using Sink = std::function<void (const juce::String&)>;

    // sink == nullptr -> write to the platform default app log file.
    // startBackgroundThread == false -> caller pumps drainNow()/flushStats()
    // manually (deterministic testing).
    explicit AudioHealthLogger (Sink sink = nullptr, bool startBackgroundThread = true);
    ~AudioHealthLogger() override;

    // Message thread. Sets the RT budget and (re)logs a prepare line. Safe to
    // call repeatedly (device/rate changes).
    void prepare (double newSampleRate, int blockSize);

    // ---- audio thread: RT-safe (push only) ---------------------------------
    void logRenderTime (float ms, std::uint64_t seq) noexcept
    {
        RtLogEvent e; e.kind = RtLogEvent::Kind::RenderTime; e.f0 = ms; e.seq = seq;
        ring.push (e);
    }
    void logOverrun (float ms, float budgetMsArg, std::uint64_t seq) noexcept
    {
        RtLogEvent e; e.kind = RtLogEvent::Kind::Overrun; e.f0 = ms; e.f1 = budgetMsArg; e.seq = seq;
        ring.push (e);
    }
    void logVoiceCount (int n) noexcept
    {
        RtLogEvent e; e.kind = RtLogEvent::Kind::VoiceCount; e.i0 = n;
        ring.push (e);
    }
    void logSteals (int n) noexcept
    {
        if (n <= 0) return;
        RtLogEvent e; e.kind = RtLogEvent::Kind::Steals; e.i0 = n;
        ring.push (e);
    }

    // ---- message thread: direct (non-RT) logging ---------------------------
    void logMessage (const juce::String& text) { emit (text); }

    // ---- overlay snapshot (message thread) ---------------------------------
    struct Snapshot
    {
        float          medianMs = 0, p99Ms = 0, maxMs = 0, budgetMs = 0;
        int            voiceHighWater = 0, stealsPerPeriod = 0, overruns = 0;
        std::uint64_t  dropped = 0;
        double         cpuPercent = 0;    // p99 / budget * 100
    };
    Snapshot snapshot() const noexcept;

    // ---- test/support hooks (also used by the drain thread) ----------------
    void drainNow();       // pop + accumulate all queued events
    void flushStats();     // compute + log the current window, then reset

    std::uint64_t droppedCount() const noexcept { return ring.droppedCount(); }

private:
    void run() override;                   // background drain loop
    void emit (const juce::String& s);     // route to sink or file logger

    RtLogRing<4096> ring;
    Sink sink;
    std::unique_ptr<juce::FileLogger> fileLogger;

    double budgetMs = 2.667;

    // drain-thread window accumulators
    std::vector<float> renderTimes;
    int windowVoiceHighWater = 0, windowSteals = 0, windowOverruns = 0;

    // overlay atomics (drain writes, message reads)
    std::atomic<float> aMedian { 0 }, aP99 { 0 }, aMax { 0 }, aBudget { 2.667f };
    std::atomic<int>   aVoiceHW { 0 }, aSteals { 0 }, aOverrunsTotal { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioHealthLogger)
};
