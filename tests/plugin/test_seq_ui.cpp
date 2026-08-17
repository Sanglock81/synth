// Synth — Copyright © 2026 John L Farmer. Licensed under AGPLv3; see LICENSE.
// ============================================================================
// SeqPanel UI interaction (#54): the step-sequencer cells share ONE grammar with the
// arp — a single tap TOGGLES a cell (dark->on, lit->off); touch-and-hold + vertical drag
// sets its velocity %. Driven through the real mouse handlers.
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

TEST_CASE ("seq UI: single tap toggles a cell on then off", "[plugin][seq][ui]")
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
    REQUIRE (proc.getSeqCell (1, 2) == 0);          // a second tap on the LIT cell turns it OFF
}

TEST_CASE ("seq UI: a single tap on a lit cell turns it off", "[plugin][seq][ui]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VASynthProcessor proc;
    SeqPanel panel (proc);
    panel.setSize (900, 300);

    proc.setSeqCell (2, 5, 1);
    tap (panel, 2, 5);                              // one quick tap silences the step (no double-tap needed)
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

// ---------------------------------------------------------------------------
// B6 — the grid's DEFAULT ROWS. The eight a drummer reaches for first, on this
// project's own kit trigger notes, labelled by what they will actually trigger.
// ---------------------------------------------------------------------------

TEST_CASE ("seq default rows: the foundational eight reach the live sequencer", "[plugin][seq][defaults]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;

    // A FRESH processor, i.e. what a first-run user sees. This is the assertion that would
    // have caught the old defect: the declared default was {36,38,40,...} but the state read
    // fell back to a chromatic 36..43 run when seq_notes was absent, which it always is on a
    // fresh state — so the panel shipped showing notes nobody had chosen.
    const int want[] { 36, 38, 39, 42, 43, 48, 47, 44 };
    for (int r = 0; r < VASynthProcessor::kSeqRows; ++r)
    { INFO ("row " << r); REQUIRE (p.getSeqNote (r) == want[r]); }
}

TEST_CASE ("seq default rows: labels name the pad each row triggers", "[plugin][seq][defaults][ui]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    SeqPanel panel (p);
    panel.setSize (1000, 320);

    // The labels must follow the KIT map, not GM — GM calls 39 a clap and 43 a ride, and both
    // are wrong for every kit in this library. A grid that mislabels its own rows is worse than
    // one with no labels at all.
    const char* want[] { "Kick", "Snare", "Rim", "Hat", "OpHat", "Crash", "Ride", "Tom Lo" };
    for (int r = 0; r < VASynthProcessor::kSeqRows; ++r)
    {
        INFO ("row " << r << " note " << p.getSeqNote (r));
        REQUIRE (SeqPanel::rowLabelForTest (p.getSeqNote (r)).startsWith (want[r]));
    }
}

TEST_CASE ("seq default rows: a saved pattern keeps its own notes", "[plugin][seq][defaults][state]")
{
    // Changing the DEFAULT must not reach back into patterns people already have. A saved
    // state carries seq_notes, so it restores exactly what was saved, defaults or not.
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor a;
    const int custom[] { 60, 61, 62, 63, 64, 65, 66, 67 };
    for (int r = 0; r < VASynthProcessor::kSeqRows; ++r) a.setSeqNote (r, custom[r]);
    a.setSeqCell (0, 0, 1);
    juce::MemoryBlock state;
    a.getStateInformation (state);

    VASynthProcessor b;
    b.setStateInformation (state.getData(), (int) state.getSize());
    for (int r = 0; r < VASynthProcessor::kSeqRows; ++r)
    { INFO ("row " << r); REQUIRE (b.getSeqNote (r) == custom[r]); }
    REQUIRE (b.getSeqCell (0, 0) != 0);
}

TEST_CASE ("seq default rows: screenshot of the grid's default row labels", "[plugin][seq][defaults][smoke]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    SeqPanel panel (p);
    panel.setSize (1000, 320);
    // A four-on-the-floor with hats and a backbeat, so the shot shows the rows in use rather
    // than an empty grid — the point of review is that each row is named for what it triggers.
    for (int s = 0; s < 16; s += 4)  p.setSeqCell (0, s, 1);          // kick
    for (int s = 4; s < 16; s += 8)  p.setSeqCell (1, s, 1);          // snare
    for (int s = 0; s < 16; s += 2)  p.setSeqCell (3, s, 1);          // closed hat
    p.setSeqCell (4, 14, 1);                                          // open hat lift
    p.setSeqCell (5, 0, 1);                                           // crash on the one
    for (int s = 2; s < 16; s += 4)  p.setSeqCell (6, s, 1);          // ride
    p.setSeqCell (7, 11, 1);                                          // tom fill

    auto img = panel.createComponentSnapshot (panel.getLocalBounds(), false, 1.0f);
    REQUIRE (img.isValid());
    juce::File out (juce::String (VASYNTH_DOCS_DIR) + "/smoke/seq-default-rows.png");
    out.getParentDirectory().createDirectory();
    out.deleteFile();
    juce::FileOutputStream os (out); REQUIRE (os.openedOk());
    juce::PNGImageFormat png; REQUIRE (png.writeImageToStream (img, os));
}
