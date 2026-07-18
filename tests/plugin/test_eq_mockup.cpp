// ============================================================================
// K1 sign-off MOCKUP (non-functional): the proposed consolidated per-part EQ section —
// vertical GAIN sliders, one per band, at the size the right-column section actually gets.
// Renders to docs/smoke/eq-section-mockup.png for review BEFORE any wiring (layout rule).
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include "UI/VASynthLookAndFeel.h"

#ifndef VASYNTH_DOCS_DIR
 #define VASYNTH_DOCS_DIR "."
#endif

namespace
{
    struct EqSectionMock : juce::Component
    {
        void paint (juce::Graphics& g) override
        {
            using LF = VASynthLookAndFeel;
            auto r = getLocalBounds();
            g.setColour (LF::panelLight().darker (0.12f));
            g.fillRoundedRectangle (r.toFloat(), 6.0f);

            // Header carries the PART name so it's obvious whose EQ this shapes.
            auto head = r.removeFromTop (24);
            g.setColour (juce::Colour (0xff67c0c8));                 // the viz/EQ tint
            g.fillRoundedRectangle (head.toFloat(), 6.0f); g.fillRect (head.withTop (head.getCentreY()));
            g.setColour (juce::Colour (0xff0e1319));
            g.setFont (juce::Font (juce::FontOptions (12.5f, juce::Font::bold)));
            g.drawText ("  EQ  -  P1 LIVE", head, juce::Justification::centredLeft, false);

            auto foot = r.removeFromBottom (16);
            g.setColour (LF::dim());
            g.setFont (juce::Font (juce::FontOptions (10.0f)));
            g.drawText ("drag <-> freq   double-tap = value   dot = band on/off",
                        foot.reduced (8, 0), juce::Justification::centred, false);

            auto body = r.reduced (8, 6);
            const char* names[] { "LOW", "L-MID", "H-MID", "HIGH" };
            const char* freqs[] { "120 Hz", "500 Hz", "3.0 kHz", "8.0 kHz" };
            const float gains[] { 0.62f, 0.5f, 0.38f, 0.55f };       // 0.5 = 0 dB (centre)
            const bool  on[]    { true, true, false, true };
            const char* gtxt[]  { "+3.0", "0.0", "-4.5", "+2.0" };
            const int n = 4, bw = body.getWidth() / n;
            for (int i = 0; i < n; ++i)
            {
                auto col = juce::Rectangle<int> (body.getX() + i * bw, body.getY(), bw, body.getHeight()).reduced (3, 0);
                // on/off dot
                auto dot = col.removeFromTop (14);
                g.setColour (on[i] ? juce::Colour (0xff67c0c8) : LF::track().brighter (0.1f));
                g.fillEllipse (dot.withSizeKeepingCentre (10, 10).toFloat());
                // band name
                g.setColour (on[i] ? LF::ink() : LF::dim());
                g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
                g.drawText (names[i], col.removeFromTop (14), juce::Justification::centred, false);
                // gain readout
                auto val = col.removeFromBottom (13);
                g.setColour (LF::accent());
                g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 10.5f, juce::Font::bold)));
                g.drawText (juce::String (gtxt[i]) + " dB", val, juce::Justification::centred, false);
                // freq label beneath the slider (freq is set by horizontal drag; no knob)
                auto flab = col.removeFromBottom (13);
                g.setColour (LF::dim());
                g.setFont (juce::Font (juce::FontOptions (10.0f)));
                g.drawText (freqs[i], flab, juce::Justification::centred, false);
                // vertical gain slider (track + 0 dB centre line + thumb)
                auto track = col.withSizeKeepingCentre (16, col.getHeight());
                g.setColour (LF::track().darker (0.25f));
                g.fillRoundedRectangle (track.toFloat(), 5.0f);
                g.setColour (LF::dim().withAlpha (0.5f));
                g.drawHorizontalLine (track.getCentreY(), (float) track.getX() - 3, (float) track.getRight() + 3);   // 0 dB
                const int ty = track.getY() + (int) ((1.0f - gains[i]) * (track.getHeight() - 18));
                g.setColour (on[i] ? LF::accent() : LF::dim());
                g.fillRoundedRectangle (juce::Rectangle<int> (track.getX() - 5, ty, track.getWidth() + 10, 16).toFloat(), 3.0f);
            }
        }
    };
}

TEST_CASE ("K1 EQ-section mockup renders for sign-off", "[plugin][mockup][eq][screenshot]")
{
    juce::ScopedJuceInitialiser_GUI init;
    EqSectionMock m; m.setSize (286, 210);   // the right-column EQ section's actual size
    auto img = m.createComponentSnapshot (m.getLocalBounds(), false, 2.0f);   // 2x for legibility
    REQUIRE (img.isValid());
    juce::File out (juce::String (VASYNTH_DOCS_DIR) + "/smoke/eq-section-mockup.png");
    out.getParentDirectory().createDirectory(); out.deleteFile();
    juce::FileOutputStream os (out); REQUIRE (os.openedOk());
    juce::PNGImageFormat png; REQUIRE (png.writeImageToStream (img, os));
}
