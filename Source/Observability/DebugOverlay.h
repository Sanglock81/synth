// Synth — Copyright © 2026 John L Farmer. Licensed under AGPLv3; see LICENSE.
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
        if (isVisible()) startTimerHz (30);        // ~mod-animation rate: the live count pulses with the notes
        else             stopTimer();
    }

    // Optional extra diagnostic lines (G6: the live pitch-bend/mod-wheel intake trace).
    // Kept decoupled from the processor — the editor supplies a provider.
    void setExtraLinesProvider (std::function<juce::StringArray()> fn) { extraLines = std::move (fn); }

    // The running build banner (version + build-fresh git hash) — so the exact binary is confirmable.
    void setVersionLine (juce::String v) { versionLine = std::move (v); }
    juce::String versionLineText() const { return versionLine; }   // test hook

    void paint (juce::Graphics& g) override
    {
        const auto s = health.snapshot();

        g.fillAll (juce::Colours::black.withAlpha (0.72f));
        g.setColour (juce::Colours::limegreen);
        g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                                  13.0f, juce::Font::plain)));

        if (versionLine.isNotEmpty())
        {
            g.setColour (juce::Colours::aqua);
            g.drawText (versionLine, 8, 6, getWidth() - 16, 16, juce::Justification::left);
        }

        const int total = s.voicesLive + s.voicesGen + s.voicesSmp;
        juce::StringArray lines;
        lines.add ("CPU  " + juce::String (s.cpuPercent, 1) + "%   p99 "
                   + juce::String (s.p99Ms, 3) + " / " + juce::String (s.budgetMs, 3) + " ms");
        // LIVE (played) / GEN (arp/seq/looper) / SMP (sample pads) — instantaneous, broken out so a
        // leak's origin is visible. hi<= is the 10 s high-water (context, not the live value).
        lines.add ("voices " + juce::String (total)
                   + "  LIVE " + juce::String (s.voicesLive)
                   + "  GEN " + juce::String (s.voicesGen)
                   + "  SMP " + juce::String (s.voicesSmp)
                   + "   hi<=" + juce::String (s.voiceHighWater) + " steals/10s " + juce::String (s.stealsPerPeriod));
        lines.add ("overruns " + juce::String (s.overruns)
                   + "   log-drops " + juce::String ((juce::int64) s.dropped));

        g.setColour (juce::Colours::limegreen);
        int y = versionLine.isNotEmpty() ? 24 : 6;
        for (auto& l : lines) { g.drawText (l, 8, y, getWidth() - 16, 16, juce::Justification::left); y += 17; }

        // Master output meter beside the counts: makes "sound at 0 voices" instantly attributable
        // (a legit FX/reverb tail shows level with total==0; a counting HOLE shows level AND notes
        // audibly playing but total stuck low). Peak-held for readability.
        meterHeld = std::max (s.masterPeak, meterHeld * 0.90f);   // ~30 Hz decay
        {
            const int mx = 8, mw = getWidth() - 16, mh = 9;
            g.setColour (juce::Colours::darkgrey.withAlpha (0.8f));
            g.fillRect (mx, y, mw, mh);
            const float lvl = juce::jlimit (0.0f, 1.0f, meterHeld);
            g.setColour (meterHeld > 0.99f ? juce::Colours::red
                                           : (meterHeld > 0.7f ? juce::Colours::orange : juce::Colours::limegreen));
            g.fillRect (mx, y, (int) (mw * lvl), mh);
            g.setColour (juce::Colours::white.withAlpha (0.85f));
            g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 10.0f, juce::Font::plain)));
            g.drawText ("MASTER " + juce::String (s.masterPeak, 2), mx + 2, y - 1, mw - 4, mh + 2, juce::Justification::right);
        }
        y += 15;

        // Saturation-activity indicator: the safety clipper engaged this window.
        // Amber when active (with the sample count), dim grey when clean.
        if (s.clipActive) g.setColour (juce::Colours::orange);
        else              g.setColour (juce::Colours::grey);
        g.drawText (s.clipActive ? ("SAT  " + juce::String ((juce::int64) s.clipSamples) + " smpl")
                                 : juce::String ("SAT  --"),
                    8, y, getWidth() - 16, 16, juce::Justification::left);
        y += 17;

        if (extraLines)
        {
            g.setColour (juce::Colours::aqua);
            for (auto& l : extraLines()) { g.drawText (l, 8, y, getWidth() - 16, 16, juce::Justification::left); y += 17; }
        }
    }

private:
    void timerCallback() override { repaint(); }   // only runs while visible

    AudioHealthLogger& health;
    std::function<juce::StringArray()> extraLines;
    juce::String versionLine;
    float meterHeld = 0.0f;      // peak-hold for the master meter (decays each repaint)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DebugOverlay)
};
