// ============================================================================
// AudioHealthLogger: the background drain formats POD events into log lines, and
// exposes a message-thread snapshot for the debug overlay. Driven synchronously
// (no background thread) so the assertions are deterministic.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <juce_events/juce_events.h>          // ScopedJuceInitialiser_GUI
#include "Observability/AudioHealthLogger.h"
#include <vector>

namespace
{
    struct Capture
    {
        std::vector<juce::String> lines;
        AudioHealthLogger::Sink sink() { return [this] (const juce::String& s) { lines.push_back (s); }; }
        bool has (const juce::String& sub) const
        {
            for (auto& l : lines) if (l.contains (sub)) return true;
            return false;
        }
        juce::String find (const juce::String& sub) const
        {
            for (auto& l : lines) if (l.contains (sub)) return l;
            return {};
        }
    };
}

TEST_CASE ("drain formats a render-time stats line with the right percentiles", "[obs][health]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    Capture cap;
    AudioHealthLogger log (cap.sink(), /*startBackgroundThread*/ false);
    log.prepare (48000.0, 128);                        // 2.667 ms budget

    // 100 render times 1.00..1.99 ms.
    for (int i = 0; i < 100; ++i) log.logRenderTime (1.0f + 0.01f * i, (std::uint64_t) i);
    log.drainNow();
    log.flushStats();

    REQUIRE (cap.has ("render ms"));
    const auto line = cap.find ("render ms");
    INFO (line);
    REQUIRE (line.contains ("min=1.000"));
    REQUIRE (line.contains ("max=1.990"));
    REQUIRE (line.contains ("n=100"));

    auto snap = log.snapshot();
    REQUIRE (snap.medianMs == Catch::Approx (1.50f).margin (0.02));
    REQUIRE (snap.p99Ms    == Catch::Approx (1.99f).margin (0.02));
    REQUIRE (snap.budgetMs == Catch::Approx (2.667f).margin (0.01));
    REQUIRE (snap.cpuPercent == Catch::Approx (1.99 / 2.667 * 100.0).margin (1.0));
}

TEST_CASE ("overrun events are logged immediately with the measured time", "[obs][health][overrun]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    Capture cap;
    AudioHealthLogger log (cap.sink(), false);
    log.prepare (48000.0, 128);

    log.logOverrun (4.2f, 2.667f, 4242);
    log.drainNow();

    REQUIRE (cap.has ("OVERRUN"));
    const auto line = cap.find ("OVERRUN");
    INFO (line);
    REQUIRE (line.contains ("block 4242"));
    REQUIRE (line.contains ("4.200 ms"));
    REQUIRE (line.contains ("2.667 ms budget"));
}

TEST_CASE ("voice high-water and steals aggregate over the window", "[obs][health][voices]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    Capture cap;
    AudioHealthLogger log (cap.sink(), false);
    log.prepare (48000.0, 128);

    log.logVoiceCount (3); log.logVoiceCount (11); log.logVoiceCount (7);   // high-water 11
    log.logSteals (2); log.logSteals (1);                                   // 3 total
    log.logRenderTime (1.0f, 0);                                            // so stats emit
    log.drainNow();
    log.flushStats();

    auto snap = log.snapshot();
    REQUIRE (snap.voiceHighWater == 11);
    REQUIRE (snap.stealsPerPeriod == 3);

    const auto line = cap.find ("render ms");
    REQUIRE (line.contains ("voices<=11"));
    REQUIRE (line.contains ("steals=3"));
}
