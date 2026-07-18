#pragma once
#include <juce_core/juce_core.h>
#include <cmath>

// ============================================================================
// Mod-indicator MOTION model (#56 follow-up, H1). The connect animation must read as
// an ECHO of a value in MOTION, not merely "a route exists": a control with a static
// routed offset (e.g. a macro parked at a value) shows NOTHING at rest. The indicator
// lights while the modulated value is CHANGING and DECAYS to nothing shortly after it
// goes still; motion resuming re-lights it immediately. All tuning lives here.
//
// State is pure logic (timestamps passed in) so it is unit-testable without a message
// loop; the overlays feed it the live modAnim() offset at kTimerHz and repaint on change.
// ============================================================================
namespace modanim
{
    constexpr float kMotionEps = 0.0025f;   // |delta| at/below this per tick reads as "static"
    constexpr int   kHoldMs    = 200;       // stay lit this long after motion stops, THEN decay
    constexpr int   kDecayMs   = 700;       // fade-out duration once decay begins
    constexpr float kEchoLag   = 0.35f;     // trailing-echo smoothing per tick (0..1; higher = snappier)
    constexpr int   kTimerHz   = 30;
    constexpr float kVisibleEps = 0.01f;    // intensity below this renders nothing

    struct State
    {
        float cur = 0.0f, lag = 0.0f, intensity = 0.0f;
        juce::uint32 lastMotionMs = 0;
        bool primed = false;

        // Feed the current modulated offset + a monotonic ms clock. Returns true if a repaint
        // is warranted (visible now, or just went dark this frame).
        bool tick (float sample, juce::uint32 nowMs)
        {
            const float prev = cur;
            cur = sample;
            if (! primed) { lag = cur; primed = true; lastMotionMs = nowMs; }   // no phantom flare on first sample
            else          lag += (cur - lag) * kEchoLag;

            const bool wasVisible = intensity > kVisibleEps;
            const float delta = std::abs (cur - prev);
            if (delta > kMotionEps)                                  // moving -> full, re-light immediately
            {
                lastMotionMs = nowMs;
                intensity = 1.0f;
            }
            else if (nowMs - lastMotionMs > (juce::uint32) kHoldMs)  // still past the hold -> decay
            {
                intensity = juce::jmax (0.0f, intensity - (1000.0f / kTimerHz) / (float) kDecayMs);
            }
            const bool visible = intensity > kVisibleEps;
            return visible || wasVisible;
        }

        bool  visible() const { return intensity > kVisibleEps; }
        float alpha()   const { return juce::jlimit (0.0f, 1.0f, intensity); }
    };
}
