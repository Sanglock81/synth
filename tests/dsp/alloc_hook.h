#pragma once
#include <cstddef>
#include <atomic>

// ============================================================================
// Global-new allocation counter for the real-time-safety test.
//
// The DSP test binary overrides global operator new/delete (see alloc_hook.cpp)
// to increment counters. Counting is normally OFF so Catch2's own allocations
// don't register; arm it around the audio-thread code under test.
//
//   AllocGuard g;              // arms counting, resets to zero
//   engine.render(...);        // <-- must not allocate
//   REQUIRE(g.count() == 0);
// ============================================================================

namespace alloc_hook
{
    extern std::atomic<bool>        counting;
    extern std::atomic<std::size_t> newCount;
    extern std::atomic<std::size_t> deleteCount;

    struct AllocGuard
    {
        AllocGuard()
        {
            newCount.store (0, std::memory_order_relaxed);
            deleteCount.store (0, std::memory_order_relaxed);
            counting.store (true, std::memory_order_release);
        }
        ~AllocGuard() { counting.store (false, std::memory_order_release); }

        std::size_t count()    const { return newCount.load (std::memory_order_acquire); }
        std::size_t frees()    const { return deleteCount.load (std::memory_order_acquire); }
    };
}
