// ============================================================================
// R2 touch: GRAB-mode controls. A first touch on a fader/knob must ACQUIRE the
// control with ZERO value change; the value moves only on drag, relative to the
// grab point (snap-to-position caused unexpected jumps during live play).
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include "UI/Widgets.h"     // kDragPixelsForFullRange
#include "PluginProcessor.h"   // #139: a real RotaryKnob wired to the MIDI-learn manager

namespace
{
    // A slider configured exactly like the panel faders (LabelledFader): grab mode.
    void configureGrab (juce::Slider& s)
    {
        s.setSliderStyle (juce::Slider::LinearVertical);
        s.setVelocityBasedMode (false);
        s.setSliderSnapsToMousePosition (false);
        s.setMouseDragSensitivity (kDragPixelsForFullRange);
        s.setRange (0.0, 1.0, 0.0);
        s.setSize (40, 600);   // taller than the drag extent so travel isn't clamped
    }

    juce::MouseEvent evt (juce::Component& c, juce::Point<float> pos, juce::Point<float> downPos, bool dragged)
    {
        auto src  = juce::Desktop::getInstance().getMainMouseSource();
        auto mods = juce::ModifierKeys().withFlags (juce::ModifierKeys::leftButtonModifier);
        const auto t = juce::Time::getCurrentTime();
        return juce::MouseEvent (src, pos, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, t, downPos, t, 1, dragged);
    }
}

TEST_CASE ("grab-mode: touch-down changes nothing; drag moves relative to the grab", "[plugin][touch][grab]")
{
    juce::ScopedJuceInitialiser_GUI init;
    juce::Slider s;
    configureGrab (s);
    s.setValue (0.5, juce::dontSendNotification);

    // Touch DOWN near the bottom (a snapping slider would jump to ~0.1). Grab mode: no change.
    const juce::Point<float> down (20.0f, 180.0f);
    s.mouseDown (evt (s, down, down, false));
    REQUIRE (s.getValue() == Catch::Approx (0.5));          // ACQUIRED, zero delta

    // Drag UP 50 px (of 200) -> value rises ~0.25, relative to the grab point.
    s.mouseDrag (evt (s, { 20.0f, 130.0f }, down, true));
    const double up = s.getValue();
    REQUIRE (up > 0.5);
    REQUIRE (up == Catch::Approx (0.75).margin (0.1));       // proportional to movement only

    // Drag back DOWN past the start -> value falls below the grab value.
    s.mouseDrag (evt (s, { 20.0f, 200.0f }, down, true));
    REQUIRE (s.getValue() < 0.5);
    s.mouseUp (evt (s, { 20.0f, 200.0f }, down, true));
}

TEST_CASE ("grab-mode: config is snap-off (regression pin)", "[plugin][touch][grab]")
{
    juce::ScopedJuceInitialiser_GUI init;
    juce::Slider s; configureGrab (s);
    REQUIRE_FALSE (s.getSliderSnapsToMousePosition());
}

TEST_CASE ("grab-mode: drag sensitivity = kDragPixelsForFullRange px for full range", "[plugin][touch][grab][sens]")
{
    juce::ScopedJuceInitialiser_GUI init;
    juce::Slider s; configureGrab (s);
    s.setValue (0.1, juce::dontSendNotification);

    // Drag UP exactly HALF the full-range extent -> the value must rise by ~0.5. This
    // pins the tuned sensitivity: JUCE's default 250 px would give a different (larger)
    // delta, so a lost/reverted setMouseDragSensitivity fails here.
    const juce::Point<float> down (20.0f, 300.0f);
    s.mouseDown (evt (s, down, down, false));
    const float halfExtentPx = (float) kDragPixelsForFullRange * 0.5f;
    s.mouseDrag (evt (s, { 20.0f, 300.0f - halfExtentPx }, down, true));
    REQUIRE (s.getValue() == Catch::Approx (0.6).margin (0.04));   // 0.1 + 0.5
}

// #139: an accidental stationary long-press arms MIDI-learn (amber "yellow" highlight) and it
// used to stay stuck on until a CC arrived. Fix: tapping an armed control CANCELS the arm.
// Without the mouseDown guard this tap would just re-start the long-press timer and the param
// would stay armed -> REQUIRE_FALSE fails; with it, the arm (and its highlight) clears.
TEST_CASE ("learn-arm is cancelled by tapping the armed control (#139)", "[plugin][touch][midilearn]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 64);

    RotaryKnob knob (p.apvts, "macro1", "M1", p.getMidiLearn());
    knob.setSize (40, 40);

    p.getMidiLearn().armLearn ("macro1");
    REQUIRE (p.getMidiLearn().isLearningParam ("macro1"));        // armed: amber pulse

    const juce::Point<float> at (5.0f, 5.0f);
    knob.mouseDown (evt (knob, at, at, false));                   // tap the armed control

    REQUIRE_FALSE (p.getMidiLearn().isLearningParam ("macro1"));  // cancelled -> highlight clears
}
