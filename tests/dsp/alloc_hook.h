#pragma once
#include <cstddef>
#include <atomic>

// ============================================================================
// Allocation counter for the real-time-safety tests.
//
// The test binaries override global operator new/delete (see alloc_hook.cpp) to
// count heap allocations. Counting is armed PER THREAD: an AllocGuard only
// counts allocations on the thread that constructed it. This matters because a
// JUCE message thread may legitimately allocate (e.g. processing async parameter
// notifications from setValueNotifyingHost) concurrently with the audio thread —
// those are not RT violations and must not be attributed to processBlock.
//
//   AllocGuard g;              // arms counting on THIS thread, resets to zero
//   engine.render(...);        // <-- must not allocate on this thread
//   REQUIRE(g.count() == 0);
// ============================================================================

namespace alloc_hook
{
    // Per-thread arm flag: only the arming thread's allocations are counted.
    inline thread_local bool armed = false;

    extern std::atomic<std::size_t> newCount;
    extern std::atomic<std::size_t> deleteCount;

    struct AllocGuard
    {
        AllocGuard()
        {
            newCount.store (0, std::memory_order_relaxed);
            deleteCount.store (0, std::memory_order_relaxed);
            armed = true;
        }
        ~AllocGuard() { armed = false; }

        std::size_t count() const { return newCount.load (std::memory_order_acquire); }
        std::size_t frees() const { return deleteCount.load (std::memory_order_acquire); }
    };
}
