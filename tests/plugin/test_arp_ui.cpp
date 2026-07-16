// ============================================================================
// ArpBar UI interaction (#54): the arp's 16 step boxes use the SAME grammar as the
// step sequencer — tap toggles a step on/off, hold + vertical drag sets that step's
// velocity %, and a horizontal drag paints on/off across steps. Driven through the
// real mouse handlers (synthetic MouseEvents), asserting the processor state.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include "UI/BottomZones.h"

namespace
{
    juce::MouseEvent evt (juce::Component& c, juce::Point<float> pos, juce::Point<float> downPos, bool dragged)
    {
        auto src  = juce::Desktop::getInstance().getMainMouseSource();
        auto mods = juce::ModifierKeys().withFlags (juce::ModifierKeys::leftButtonModifier);
        const auto t = juce::Time::getCurrentTime();
        return juce::MouseEvent (src, pos, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, t, downPos, t, 1, dragged);
    }

    // Centre of step `s`'s cell in the arp grid, in ArpBar-local coordinates.
    juce::Point<float> cellCentre (const ArpBar& bar, int s)
    {
        auto g = bar.stepGridBounds();
        const int cw = juce::jmax (1, g.getWidth() / VASynthProcessor::kArpSteps);
        return { (float) (g.getX() + s * cw + cw / 2), (float) g.getCentreY() };
    }
}

TEST_CASE ("arp UI: tap toggles a step on/off", "[plugin][arp][ui]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VASynthProcessor proc;
    ArpBar bar (proc);
    bar.setSize (1200, 120);                       // triggers resized() -> lays out the grid
    REQUIRE (bar.stepGridBounds().getWidth() > 0);

    REQUIRE (proc.getArpStep (3) > 0.5f);          // fresh default: all steps on
    const auto p = cellCentre (bar, 3);
    bar.mouseDown (evt (bar, p, p, false));
    bar.mouseUp   (evt (bar, p, p, false));        // tap (no drag) -> toggle OFF
    REQUIRE (proc.getArpStep (3) < 0.5f);

    bar.mouseDown (evt (bar, p, p, false));
    bar.mouseUp   (evt (bar, p, p, false));        // tap again -> back ON
    REQUIRE (proc.getArpStep (3) > 0.5f);
}

TEST_CASE ("arp UI: hold + vertical drag sets the step's velocity", "[plugin][arp][ui]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VASynthProcessor proc;
    ArpBar bar (proc);
    bar.setSize (1200, 120);

    REQUIRE (proc.getArpStep (2) > 0.5f);          // on, so a hold-drag edits velocity
    REQUIRE (proc.getArpStepVel (2) == 100);       // default

    const auto down = cellCentre (bar, 2);
    bar.mouseDown (evt (bar, down, down, false));
    bar.mouseDrag (evt (bar, { down.x, down.y - 40.0f }, down, true));   // drag UP -> louder
    const int up = proc.getArpStepVel (2);
    bar.mouseUp   (evt (bar, { down.x, down.y - 40.0f }, down, true));
    REQUIRE (up > 100);

    // A fresh hold-drag DOWN takes it below 100.
    bar.mouseDown (evt (bar, down, down, false));
    bar.mouseDrag (evt (bar, { down.x, down.y + 45.0f }, down, true));   // drag DOWN -> quieter
    bar.mouseUp   (evt (bar, { down.x, down.y + 45.0f }, down, true));
    REQUIRE (proc.getArpStepVel (2) < 100);

    // The vertical velocity gesture must NOT flip the step off.
    REQUIRE (proc.getArpStep (2) > 0.5f);
}

TEST_CASE ("arp UI: horizontal drag paints on/off across steps", "[plugin][arp][ui]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VASynthProcessor proc;
    ArpBar bar (proc);
    bar.setSize (1200, 120);

    REQUIRE (proc.getArpStep (4) > 0.5f);
    REQUIRE (proc.getArpStep (6) > 0.5f);

    const auto down = cellCentre (bar, 4);
    bar.mouseDown (evt (bar, down, down, false));
    bar.mouseDrag (evt (bar, cellCentre (bar, 6), down, true));   // same y -> paint (turn off)
    bar.mouseUp   (evt (bar, cellCentre (bar, 6), down, true));

    REQUIRE (proc.getArpStep (4) < 0.5f);          // the down cell painted off
    REQUIRE (proc.getArpStep (6) < 0.5f);          // and the dragged-to cell too
}
