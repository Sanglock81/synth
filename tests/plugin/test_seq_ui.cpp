// ============================================================================
// SeqPanel UI interaction (#54): the step-sequencer cells share ONE grammar with the
// arp — single tap a DARK cell turns it on; double-tap a LIT cell turns it off (a stray
// single tap never silences a step); touch-and-hold + vertical drag sets its velocity %.
// Driven through the real mouse handlers.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include "UI/SeqPanel.h"

namespace
{
    juce::MouseEvent evt (juce::Component& c, juce::Point<float> pos, juce::Point<float> downPos, bool dragged)
    {
        auto src  = juce::Desktop::getInstance().getMainMouseSource();
        auto mods = juce::ModifierKeys().withFlags (juce::ModifierKeys::leftButtonModifier);
        const auto t = juce::Time::getCurrentTime();
        return juce::MouseEvent (src, pos, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, t, downPos, t, 1, dragged);
    }

    juce::Point<float> cell (const SeqPanel& p, int r, int s) { return p.stepCellCentre (r, s).toFloat(); }

    void tap (SeqPanel& p, int r, int s)
    {
        const auto c = cell (p, r, s);
        p.mouseDown (evt (p, c, c, false));
        p.mouseUp   (evt (p, c, c, false));
    }
}

TEST_CASE ("seq UI: single tap turns a dark cell on; a tap on a lit cell keeps it on", "[plugin][seq][ui]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VASynthProcessor proc;
    SeqPanel panel (proc);
    panel.setSize (900, 300);                       // triggers resized() -> row rects
    REQUIRE (panel.stepCellCentre (0, 0).x > 0);

    REQUIRE (proc.getSeqCell (1, 2) == 0);          // fresh default: empty grid
    tap (panel, 1, 2);
    REQUIRE (proc.getSeqCell (1, 2) != 0);          // dark -> ON

    tap (panel, 1, 2);
    REQUIRE (proc.getSeqCell (1, 2) != 0);          // tap on a LIT cell must NOT turn it off
}

TEST_CASE ("seq UI: double-tap a lit cell turns it off", "[plugin][seq][ui]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VASynthProcessor proc;
    SeqPanel panel (proc);
    panel.setSize (900, 300);

    proc.setSeqCell (2, 5, 1);
    const auto c = cell (panel, 2, 5);
    panel.mouseDoubleClick (evt (panel, c, c, false));
    REQUIRE (proc.getSeqCell (2, 5) == 0);
}

TEST_CASE ("seq UI: hold + vertical drag sets the cell's velocity, never toggling it", "[plugin][seq][ui]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VASynthProcessor proc;
    SeqPanel panel (proc);
    panel.setSize (900, 300);

    proc.setSeqCell (0, 0, 1);
    REQUIRE (proc.getSeqStepVel (0, 0) == 100);
    const auto down = cell (panel, 0, 0);
    panel.mouseDown (evt (panel, down, down, false));
    panel.mouseDrag (evt (panel, { down.x, down.y - 40.0f }, down, true));   // hold + drag UP
    const int up = proc.getSeqStepVel (0, 0);
    panel.mouseUp   (evt (panel, { down.x, down.y - 40.0f }, down, true));
    REQUIRE (up > 100);
    REQUIRE (proc.getSeqCell (0, 0) != 0);          // still ON
}
