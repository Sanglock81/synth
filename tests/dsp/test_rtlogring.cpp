// ============================================================================
// RT-safe SPSC ring logger tests. JUCE-free. The producer (audio thread) must
// never block and never allocate; on a full ring it drops and counts.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "../../Source/Observability/RtLogRing.h"
#include "alloc_hook.h"
#include <thread>
#include <atomic>
#include <chrono>

namespace
{
    RtLogEvent renderEv (float ms, std::uint64_t seq)
    {
        RtLogEvent e; e.kind = RtLogEvent::Kind::RenderTime; e.f0 = ms; e.seq = seq; return e;
    }
}

TEST_CASE ("ring preserves FIFO order", "[rtlog][ring]")
{
    RtLogRing<8> ring;
    for (int i = 0; i < 5; ++i) REQUIRE (ring.push (renderEv (float (i), (std::uint64_t) i)));

    RtLogEvent e;
    for (int i = 0; i < 5; ++i)
    {
        REQUIRE (ring.pop (e));
        REQUIRE (e.seq == (std::uint64_t) i);
        REQUIRE (e.f0 == float (i));
    }
    REQUIRE_FALSE (ring.pop (e));         // empty
}

TEST_CASE ("full ring drops without blocking and counts drops", "[rtlog][ring][drop]")
{
    RtLogRing<8> ring;                    // usable capacity = 7 (one slot reserved)
    int accepted = 0;
    for (int i = 0; i < 100; ++i) if (ring.push (renderEv (1.0f, (std::uint64_t) i))) ++accepted;

    REQUIRE (accepted == 7);
    REQUIRE (ring.droppedCount() == 93);

    // Drain one, and exactly one more push should now fit.
    RtLogEvent e; REQUIRE (ring.pop (e));
    REQUIRE (ring.push (renderEv (2.0f, 999)));
    REQUIRE (ring.droppedCount() == 93);  // unchanged
}

TEST_CASE ("push does not allocate (RT-safe producer)", "[rtlog][ring][rt]")
{
    RtLogRing<1024> ring;
    ring.push (renderEv (1.0f, 0));       // warm

    std::size_t news = 0;
    {
        alloc_hook::AllocGuard g;
        for (int i = 0; i < 10000; ++i)
        {
            ring.push (renderEv (0.5f, (std::uint64_t) i));   // includes full-ring drops
            RtLogEvent e; ring.pop (e);
        }
        news = g.count();
    }
    INFO ("allocations in 10k push/pop = " << news);
    REQUIRE (news == 0);
}

TEST_CASE ("SPSC: producer never blocks; all non-dropped events arrive in order", "[rtlog][ring][spsc]")
{
    RtLogRing<1024> ring;
    constexpr int N = 200000;
    std::atomic<bool> producerDone { false };

    // Consumer: drain concurrently, verify monotonically increasing seq.
    std::uint64_t received = 0, lastSeq = 0; bool ordered = true;
    std::thread consumer ([&]
    {
        RtLogEvent e;
        while (! producerDone.load() || ring.sizeApprox() > 0)
        {
            if (ring.pop (e))
            {
                if (received > 0 && e.seq <= lastSeq) ordered = false;
                lastSeq = e.seq; ++received;
            }
        }
    });

    // Producer: push as fast as possible; must never block regardless of ring state.
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) ring.push (renderEv (1.0f, (std::uint64_t) (i + 1)));
    const auto t1 = std::chrono::steady_clock::now();
    producerDone.store (true);
    consumer.join();

    const double ms = std::chrono::duration<double, std::milli> (t1 - t0).count();
    INFO ("received=" << received << " dropped=" << ring.droppedCount()
          << " producer ms=" << ms);

    REQUIRE (ordered);                                        // FIFO preserved across threads
    REQUIRE (received + ring.droppedCount() == (std::uint64_t) N);   // nothing vanished
    REQUIRE (received > 0);
}
