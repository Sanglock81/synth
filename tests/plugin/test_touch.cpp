// ============================================================================
// R2 touch: GRAB-mode controls. A first touch on a fader/knob must ACQUIRE the
// control with ZERO value change; the value moves only on drag, relative to the
// grab point (snap-to-position caused unexpected jumps during live play).
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <juce_gui_basics/juce_gui_basics.h>

namespace
{
    // A slider configured exactly like the panel faders (LabelledFader): grab mode.
    void configureGrab (juce::Slider& s)
    {
        s.setSliderStyle (juce::Slider::LinearVertical);
        s.setVelocityBasedMode (false);
        s.setSliderSnapsToMousePosition (false);
        s.setRange (0.0, 1.0, 0.0);
        s.setSize (40, 200);
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
