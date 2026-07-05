#include "AudioHealthLogger.h"
#include <algorithm>

AudioHealthLogger::AudioHealthLogger (Sink sinkToUse, bool startBackgroundThread)
    : juce::Thread ("VASynth-health"), sink (std::move (sinkToUse))
{
    if (sink == nullptr)
    {
        // Platform default app log location (Linux: ~/.config per JUCE convention).
        fileLogger.reset (juce::FileLogger::createDefaultAppLogger (
            "VASynth", "VASynth.log",
            "VA Synth log — session start " + juce::Time::getCurrentTime().toString (true, true)));
        // Route JUCE's global logger (incl. the crash handler's writeToLog) here.
        juce::Logger::setCurrentLogger (fileLogger.get());
    }
    renderTimes.reserve (8192);
    if (startBackgroundThread)
        startThread (juce::Thread::Priority::low);
}

AudioHealthLogger::~AudioHealthLogger()
{
    signalThreadShouldExit();
    notify();
    stopThread (1000);
    drainNow();          // best-effort final drain
    flushStats();

    if (fileLogger != nullptr && juce::Logger::getCurrentLogger() == fileLogger.get())
        juce::Logger::setCurrentLogger (nullptr);
}

void AudioHealthLogger::prepare (double newSampleRate, int blockSize)
{
    budgetMs = (newSampleRate > 0.0) ? (double (blockSize) / newSampleRate * 1000.0) : 2.667;
    aBudget.store ((float) budgetMs);
    emit ("prepareToPlay: " + juce::String (newSampleRate, 0) + " Hz, block " + juce::String (blockSize)
          + " (" + juce::String (budgetMs, 3) + " ms budget)");
}

void AudioHealthLogger::emit (const juce::String& text)
{
    if (sink != nullptr)          sink (text);
    else if (fileLogger != nullptr) fileLogger->logMessage (text);
}

void AudioHealthLogger::run()
{
    auto lastStats = juce::Time::getMillisecondCounter();
    while (! threadShouldExit())
    {
        drainNow();
        const auto now = juce::Time::getMillisecondCounter();
        if (now - lastStats >= 10000)     // every ~10 s
        {
            flushStats();
            lastStats = now;
        }
        wait (200);
    }
}

void AudioHealthLogger::drainNow()
{
    RtLogEvent e;
    while (ring.pop (e))
    {
        switch (e.kind)
        {
            case RtLogEvent::Kind::RenderTime:
                renderTimes.push_back (e.f0);
                break;

            case RtLogEvent::Kind::Overrun:
                ++windowOverruns;
                emit ("OVERRUN block " + juce::String (e.seq) + ": "
                      + juce::String (e.f0, 3) + " ms > " + juce::String (e.f1, 3) + " ms budget");
                break;

            case RtLogEvent::Kind::VoiceCount:
                windowVoiceHighWater = std::max (windowVoiceHighWater, e.i0);
                break;

            case RtLogEvent::Kind::Steals:
                windowSteals += e.i0;
                break;

            case RtLogEvent::Kind::Marker:
                emit ("marker " + juce::String (e.i0));
                break;
        }
    }
}

void AudioHealthLogger::flushStats()
{
    if (! renderTimes.empty())
    {
        std::sort (renderTimes.begin(), renderTimes.end());
        const auto n = renderTimes.size();
        auto pct = [&] (double p) { return renderTimes[(std::size_t) (p / 100.0 * double (n - 1))]; };

        const float mn = renderTimes.front(), mx = renderTimes.back();
        const float med = pct (50.0), p99 = pct (99.0);

        aMedian.store (med); aP99.store (p99); aMax.store (mx);
        aVoiceHW.store (windowVoiceHighWater); aSteals.store (windowSteals);

        emit ("render ms  min=" + juce::String (mn, 3) + " med=" + juce::String (med, 3)
              + " p99=" + juce::String (p99, 3) + " max=" + juce::String (mx, 3)
              + " (" + juce::String (p99 / budgetMs * 100.0, 1) + "% budget)"
              + "  voices<=" + juce::String (windowVoiceHighWater)
              + "  steals=" + juce::String (windowSteals)
              + "  overruns=" + juce::String (windowOverruns)
              + "  dropped=" + juce::String ((juce::int64) ring.droppedCount())
              + "  n=" + juce::String ((juce::int64) n));
    }

    aOverrunsTotal.fetch_add (windowOverruns);
    renderTimes.clear();
    windowVoiceHighWater = 0;
    windowSteals = 0;
    windowOverruns = 0;
}

AudioHealthLogger::Snapshot AudioHealthLogger::snapshot() const noexcept
{
    Snapshot s;
    s.medianMs        = aMedian.load();
    s.p99Ms           = aP99.load();
    s.maxMs           = aMax.load();
    s.budgetMs        = aBudget.load();
    s.voiceHighWater  = aVoiceHW.load();
    s.stealsPerPeriod = aSteals.load();
    s.overruns        = aOverrunsTotal.load();
    s.dropped         = ring.droppedCount();
    s.cpuPercent      = s.budgetMs > 0.0f ? double (s.p99Ms) / double (s.budgetMs) * 100.0 : 0.0;
    return s;
}
