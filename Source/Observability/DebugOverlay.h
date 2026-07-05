#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "AudioHealthLogger.h"

// ============================================================================
// Minimal always-on-top debug overlay: CPU %, voice high-water, steals,
// overruns, and log-drop counter — readable while playing. Toggled by the
// editor (F12). Refreshes on a modest ~10 Hz timer (only while visible); it
// never touches the audio thread — it just reads AudioHealthLogger::snapshot().
// Reusable across the generic (Phase 4) and custom (Phase 5) editors.
// ============================================================================

class DebugOverlay : public juce::Component,
                     private juce::Timer
{
public:
    explicit DebugOverlay (AudioHealthLogger& healthToRead) : health (healthToRead)
    {
        setInterceptsMouseClicks (false, false);   // never block the controls beneath
        setWantsKeyboardFocus (false);             // never steal QWERTY note focus
    }

    void visibilityChanged() override
    {
        if (isVisible()) startTimerHz (10);
        else             stopTimer();
    }

    void paint (juce::Graphics& g) override
    {
        const auto s = health.snapshot();

        g.fillAll (juce::Colours::black.withAlpha (0.72f));
        g.setColour (juce::Colours::limegreen);
        g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                                  13.0f, juce::Font::plain)));

        juce::StringArray lines;
        lines.add ("CPU  " + juce::String (s.cpuPercent, 1) + "%   p99 "
                   + juce::String (s.p99Ms, 3) + " / " + juce::String (s.budgetMs, 3) + " ms");
        lines.add ("voices<=" + juce::String (s.voiceHighWater)
                   + "   steals/10s " + juce::String (s.stealsPerPeriod));
        lines.add ("overruns " + juce::String (s.overruns)
                   + "   log-drops " + juce::String ((juce::int64) s.dropped));

        int y = 6;
        for (auto& l : lines) { g.drawText (l, 8, y, getWidth() - 16, 16, juce::Justification::left); y += 17; }
    }

private:
    void timerCallback() override { repaint(); }   // only runs while visible

    AudioHealthLogger& health;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DebugOverlay)
};
