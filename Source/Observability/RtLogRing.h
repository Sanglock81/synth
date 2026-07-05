#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

// ============================================================================
// Real-time-safe logging primitives.
//
// The audio thread must NEVER call a logger that allocates, locks, or does I/O.
// So it only ever pushes small PLAIN-OLD-DATA events into a lock-free single-
// producer / single-consumer ring. A low-priority background thread drains the
// ring, formats the events into strings, and writes them to the log file.
//
// No strings are constructed on the audio thread; RtLogEvent carries an enum +
// a few numbers. push() never blocks and never allocates: if the ring is full
// it drops the event and bumps a counter (better a missing log line than an
// xrun). JUCE-free by design so it can be unit-tested without the framework.
// ============================================================================

struct RtLogEvent
{
    enum class Kind : std::uint8_t
    {
        RenderTime,     // f0 = render ms
        Overrun,        // f0 = render ms, f1 = budget ms
        VoiceCount,     // i0 = active voices
        Steals,         // i0 = steals since last block
        Marker          // i0 = an arbitrary marker code (e.g. prepare/reset)
    };

    Kind          kind = Kind::Marker;
    float         f0   = 0.0f;
    float         f1   = 0.0f;
    std::int32_t  i0   = 0;
    std::uint64_t seq  = 0;      // monotonic sequence (e.g. block index)
};

// Lock-free SPSC ring. Capacity must be a power of two. One producer thread
// (audio) calls push(); one consumer thread (drain) calls pop().
template <std::size_t Capacity>
class RtLogRing
{
    static_assert (Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                   "Capacity must be a power of two");

public:
    // Producer (audio thread). Never blocks, never allocates. Returns false and
    // increments the dropped counter if the ring is full.
    bool push (const RtLogEvent& e) noexcept
    {
        const auto w    = writeIdx.load (std::memory_order_relaxed);
        const auto next = (w + 1) & kMask;
        if (next == readIdx.load (std::memory_order_acquire))
        {
            dropped.fetch_add (1, std::memory_order_relaxed);
            return false;                                  // full — drop, don't block
        }
        buffer[w] = e;
        writeIdx.store (next, std::memory_order_release);
        return true;
    }

    // Consumer (drain thread). Returns false when empty.
    bool pop (RtLogEvent& out) noexcept
    {
        const auto r = readIdx.load (std::memory_order_relaxed);
        if (r == writeIdx.load (std::memory_order_acquire))
            return false;                                  // empty
        out = buffer[r];
        readIdx.store ((r + 1) & kMask, std::memory_order_release);
        return true;
    }

    std::uint64_t droppedCount() const noexcept { return dropped.load (std::memory_order_relaxed); }

    // Approximate number of queued events (consumer-side estimate).
    std::size_t sizeApprox() const noexcept
    {
        const auto w = writeIdx.load (std::memory_order_acquire);
        const auto r = readIdx.load (std::memory_order_acquire);
        return (w - r) & kMask;
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    std::array<RtLogEvent, Capacity> buffer {};
    std::atomic<std::size_t>  writeIdx { 0 };
    std::atomic<std::size_t>  readIdx  { 0 };
    std::atomic<std::uint64_t> dropped { 0 };
};
