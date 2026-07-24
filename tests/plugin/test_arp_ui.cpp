// ============================================================================
// ArpBar UI interaction (#54): the arp's 16 step boxes use ONE grammar (shared with
// the step sequencer) — a single tap TOGGLES a box (dark->on, lit->off); touch-and-hold
// + vertical drag sets that step's velocity %. Driven through the real mouse handlers.
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

    juce::Point<float> cellCentre (const ArpBar& bar, int s)
    {
        auto g = bar.stepGridBounds();
        const int cw = juce::jmax (1, g.getWidth() / VASynthProcessor::kArpSteps);
        return { (float) (g.getX() + s * cw + cw / 2), (float) g.getCentreY() };
    }

    void tap (ArpBar& bar, int s)         // single tap = down then up, no drag
    {
        const auto p = cellCentre (bar, s);
        bar.mouseDown (evt (bar, p, p, false));
        bar.mouseUp   (evt (bar, p, p, false));
    }
}

TEST_CASE ("arp UI: single tap turns a DARK box on; a tap on a lit box leaves it on", "[plugin][arp][ui]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VASynthProcessor proc;
    ArpBar bar (proc);
    bar.setSize (1200, 120);                       // triggers resized() -> lays out the grid
    REQUIRE (bar.stepGridBounds().getWidth() > 0);

    REQUIRE (proc.getArpStep (3) > 0.5f);          // fresh default: all steps on
    tap (bar, 3);
    REQUIRE (proc.getArpStep (3) < 0.5f);          // a single tap on a LIT box turns it OFF
    tap (bar, 3);
    REQUIRE (proc.getArpStep (3) > 0.5f);          // tap again -> back ON (toggle)
}

TEST_CASE ("arp UI: a single tap on a lit box turns it off", "[plugin][arp][ui]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VASynthProcessor proc;
    ArpBar bar (proc);
    bar.setSize (1200, 120);

    REQUIRE (proc.getArpStep (5) > 0.5f);
    tap (bar, 5);                                  // one quick tap silences the step (no double-tap needed)
    REQUIRE (proc.getArpStep (5) < 0.5f);
}

TEST_CASE ("arp UI: hold + vertical drag sets the step's velocity, never toggling it", "[plugin][arp][ui]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VASynthProcessor proc;
    ArpBar bar (proc);
    bar.setSize (1200, 120);

    REQUIRE (proc.getArpStep (2) > 0.5f);          // on, so a hold-drag edits velocity
    REQUIRE (proc.getArpStepVel (2) == 100);       // default

    const auto down = cellCentre (bar, 2);
    bar.mouseDown (evt (bar, down, down, false));
    bar.mouseDrag (evt (bar, { down.x, down.y - 40.0f }, down, true));   // hold + drag UP -> louder
    const int up = proc.getArpStepVel (2);
    bar.mouseUp   (evt (bar, { down.x, down.y - 40.0f }, down, true));
    REQUIRE (up > 100);
    REQUIRE (proc.getArpStep (2) > 0.5f);          // still ON — the vertical gesture never toggles

    // A fresh hold-drag DOWN takes it below 100.
    bar.mouseDown (evt (bar, down, down, false));
    bar.mouseDrag (evt (bar, { down.x, down.y + 45.0f }, down, true));   // drag DOWN -> quieter
    bar.mouseUp   (evt (bar, { down.x, down.y + 45.0f }, down, true));
    REQUIRE (proc.getArpStepVel (2) < 100);
    REQUIRE (proc.getArpStep (2) > 0.5f);
}

TEST_CASE ("arp UI: a plain tap never edits velocity", "[plugin][arp][ui]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VASynthProcessor proc;
    ArpBar bar (proc);
    bar.setSize (1200, 120);

    proc.setArpStepVel (6, 120);
    tap (bar, 6);
    REQUIRE (proc.getArpStepVel (6) == 120);       // untouched by a tap
}
