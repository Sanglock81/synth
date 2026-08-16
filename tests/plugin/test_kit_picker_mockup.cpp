// Synth — Copyright © 2026 John L Farmer. Licensed under AGPLv3; see LICENSE.
// ============================================================================
// #134 Kit-picker screenshot for the report/docs. The real picker is a JUCE PopupMenu
// (a separate top-level component the headless harness can't capture), so -- following the
// repo's mockup convention (test_eq_mockup / test_layout_mockup) -- this renders a faithful
// PopupMenu-styled list built from the LIVE data (classicKitNames / originalKitNames / user
// kits + real pad counts) to docs/smoke/kit-picker.png. If the grouping code changes, the
// image regenerates.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include "UI/VASynthLookAndFeel.h"

#ifndef VASYNTH_DOCS_DIR
 #define VASYNTH_DOCS_DIR "."
#endif

namespace
{
    struct Row { juce::String text; bool header; };

    struct KitPickerMock : juce::Component
    {
        std::vector<Row> rows;
        void paint (juce::Graphics& g) override
        {
            using LF = VASynthLookAndFeel;
            auto r = getLocalBounds();
            g.setColour (LF::panelLight().darker (0.25f));          // dark menu ground
            g.fillRoundedRectangle (r.toFloat(), 6.0f);
            g.setColour (LF::accent().withAlpha (0.5f));
            g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 6.0f, 1.0f);

            auto body = r.reduced (2, 6);
            // Title (the submenu the user opens).
            auto title = body.removeFromTop (26);
            g.setColour (LF::ink());
            g.setFont (juce::Font (juce::FontOptions (13.5f, juce::Font::bold)));
            g.drawText ("  Load drum kit", title, juce::Justification::centredLeft, false);
            body.removeFromTop (2);
            g.setColour (LF::dim().withAlpha (0.4f));
            g.fillRect (body.removeFromTop (1));
            body.removeFromTop (3);

            for (auto& row : rows)
            {
                auto line = body.removeFromTop (row.header ? 22 : 24);
                if (row.header)
                {
                    g.setColour (LF::accent().brighter (0.2f));
                    g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
                    g.drawText (row.text.toUpperCase(), line.reduced (10, 0),
                                juce::Justification::centredLeft, false);
                }
                else
                {
                    g.setColour (LF::ink().withAlpha (0.92f));
                    g.setFont (juce::Font (juce::FontOptions (12.5f)));
                    g.drawText (row.text, line.reduced (22, 0), juce::Justification::centredLeft, false);
                }
            }
        }
    };
}

TEST_CASE ("kit picker screenshot: Classic Machines / Originals / User grouping (#134)", "[plugin][drums][kits][mockup]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;

    auto padCount = [&] (const juce::String& n)
    { int c = 0; auto d = p.loadKit (n); for (int i = 0; i < 16; ++i) if (d.pads[(size_t) i].source.isNotEmpty()) ++c; return c; };
    auto kitRow = [&] (const juce::String& n) { return Row { "  " + n + "   (" + juce::String (padCount (n)) + " pads)", false }; };

    KitPickerMock m;
    m.rows.push_back ({ "Classic Machines", true });
    for (auto& n : p.classicKitNames())  m.rows.push_back (kitRow (n));
    m.rows.push_back ({ "Originals", true });
    for (auto& n : p.originalKitNames()) m.rows.push_back (kitRow (n));
    m.rows.push_back ({ "User", true });
    bool anyUser = false;
    for (auto& n : p.getKitNames()) if (! p.factoryKitNames().contains (n)) { m.rows.push_back (kitRow (n)); anyUser = true; }
    if (! anyUser) m.rows.push_back ({ "  (save a kit to add your own)", false });

    // Every classic + original kit is represented, each 16 pads.
    REQUIRE (p.classicKitNames().size() == 10);
    REQUIRE (p.originalKitNames().size() == 2);
    for (auto& n : p.classicKitNames())  REQUIRE (padCount (n) == 16);

    int h = 26 + 6 + (int) m.rows.size() * 24 + 20;
    m.setSize (330, h);

    auto img = m.createComponentSnapshot (m.getLocalBounds(), false, 2.0f);   // 2x for legibility
    juce::File out (juce::String (VASYNTH_DOCS_DIR) + "/smoke/kit-picker.png");
    out.getParentDirectory().createDirectory();
    juce::FileOutputStream os (out);
    REQUIRE (os.openedOk());
    os.setPosition (0); os.truncate();
    juce::PNGImageFormat png; REQUIRE (png.writeImageToStream (img, os));
}
