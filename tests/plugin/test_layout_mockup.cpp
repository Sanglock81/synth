// ============================================================================
// R2 layout MOCKUP (non-functional). Renders the proposed 1.0 layout so the user
// can sign off on the arrangement BEFORE any attachments are wired:
//   - slim top bar (presets, master, REC, scope toggle, indicators, help)
//   - left PART RAIL (P1-P4, activity flicker, kit-pad sub-selector seam)
//   - centre panel in signal-flow order (OSC->MIX->FILTER->ENV->LFO->FX)
//   - right SCOPE + FFT of the master (toggleable, reads while playing)
//   - bottom strip: horizontal CHORD row + collapsible RHYTHM / LOOPER zones
//     (collapsed by default; the synth panel reflows into the freed space)
// Control grammar shown: knobs (continuous), faders (ADSR + mixer), one-tap
// segmented selectors (discrete), >=56 px touch targets. Rendered to PNGs.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include "UI/VASynthLookAndFeel.h"
#include <cmath>

#ifndef VASYNTH_DOCS_DIR
 #define VASYNTH_DOCS_DIR "."
#endif

namespace
{
    using juce::Rectangle; using juce::Colour; using juce::Graphics;

    Colour panel   () { return VASynthLookAndFeel::panel(); }
    Colour panelLt () { return VASynthLookAndFeel::panelLight(); }
    Colour ink     () { return VASynthLookAndFeel::ink(); }
    Colour dim     () { return VASynthLookAndFeel::dim(); }
    Colour accent  () { return VASynthLookAndFeel::accent(); }
    Colour track   () { return VASynthLookAndFeel::track(); }

    void title (Graphics& g, Rectangle<int> r, const juce::String& t, Colour c)
    {
        g.setColour (c); g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        g.drawText (t.toUpperCase(), r, juce::Justification::centredLeft, false);
    }

    // A titled section box.
    Rectangle<int> section (Graphics& g, Rectangle<int> r, const juce::String& t, Colour tint)
    {
        g.setColour (panelLt()); g.fillRoundedRectangle (r.toFloat(), 6.0f);
        g.setColour (tint.withAlpha (0.5f)); g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 6.0f, 1.0f);
        title (g, r.reduced (8, 4).removeFromTop (16), t, tint);
        return r.reduced (8, 6).withTrimmedTop (18);
    }

    void knob (Graphics& g, Rectangle<int> r, const juce::String& label)
    {
        auto c = r.reduced (2).withSizeKeepingCentre (juce::jmin (r.getWidth() - 4, 44), juce::jmin (r.getWidth() - 4, 44));
        g.setColour (track()); g.fillEllipse (c.toFloat());
        g.setColour (accent()); g.drawEllipse (c.toFloat().reduced (1.5f), 2.5f);
        // pointer
        const auto ctr = c.toFloat().getCentre(); const float a = -2.3f;
        g.drawLine (ctr.x, ctr.y, ctr.x + std::cos (a) * c.getWidth() * 0.4f, ctr.y + std::sin (a) * c.getWidth() * 0.4f, 2.5f);
        g.setColour (dim()); g.setFont (juce::Font (juce::FontOptions (9.5f)));
        g.drawText (label, r.removeFromBottom (11), juce::Justification::centred, false);
    }

    void fader (Graphics& g, Rectangle<int> r, const juce::String& label, float v)
    {
        auto col = r.withSizeKeepingCentre (10, r.getHeight() - 12).withTrimmedBottom (10);
        g.setColour (track()); g.fillRoundedRectangle (col.toFloat(), 4.0f);
        auto thumbY = col.getY() + (int) ((1.0f - v) * col.getHeight());
        g.setColour (accent()); g.fillRoundedRectangle (Rectangle<int> (col.getX() - 4, thumbY - 5, 18, 10).toFloat(), 3.0f);
        g.setColour (dim()); g.setFont (juce::Font (juce::FontOptions (9.5f)));
        g.drawText (label, r.removeFromBottom (11), juce::Justification::centred, false);
    }

    void selector (Graphics& g, Rectangle<int> r, juce::StringArray opts, int sel)
    {
        const int w = r.getWidth() / juce::jmax (1, opts.size());
        for (int i = 0; i < opts.size(); ++i)
        {
            auto cell = Rectangle<int> (r.getX() + i * w, r.getY(), w, r.getHeight()).reduced (1);
            g.setColour (i == sel ? accent() : track()); g.fillRoundedRectangle (cell.toFloat(), 3.0f);
            g.setColour (i == sel ? Colour (0xff10161c) : ink()); g.setFont (juce::Font (juce::FontOptions (10.0f)));
            g.drawText (opts[i], cell, juce::Justification::centred, false);
        }
    }

    // ---- the mockup ----------------------------------------------------------
    struct LayoutMockup : public juce::Component
    {
        void paint (Graphics& g) override
        {
            g.fillAll (panel());
            auto area = getLocalBounds();
            const auto tOsc = accent(), tFilt = Colour (0xff6ea8ff), tEnv = Colour (0xffb07cff),
                       tLfo = Colour (0xfff0a04b), tFx = Colour (0xff5ecb8a), tChord = Colour (0xffe0733a);

            // --- slim top bar ---------------------------------------------------
            auto top = area.removeFromTop (46);
            g.setColour (panelLt()); g.fillRect (top);
            auto tb = top.reduced (8, 7);
            auto preset = tb.removeFromLeft (240); g.setColour (track()); g.fillRoundedRectangle (preset.toFloat(), 5.0f);
            g.setColour (ink()); g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
            g.drawText ("  synth      Fat Saw Bass", preset, juce::Justification::centredLeft, false);
            auto help = tb.removeFromRight (34); g.setColour (track()); g.fillRoundedRectangle (help.toFloat(), 5.0f);
            g.setColour (ink()); g.drawText ("?", help, juce::Justification::centred, false);
            auto ind = tb.removeFromRight (230);
            g.setColour (dim()); g.setFont (juce::Font (juce::FontOptions (11.0f)));
            g.drawText ("CPU 12%    MIDI ok    CLK 120 int    SCOPE [on]", ind, juce::Justification::centredRight, false);
            auto rec = tb.removeFromRight (70); g.setColour (Colour (0xffd8443a)); g.fillEllipse (rec.removeFromLeft (16).toFloat().reduced (2));
            g.setColour (ink()); g.drawText ("REC", rec, juce::Justification::centredLeft, false);
            knob (g, tb.removeFromRight (60), "MASTER");

            area.removeFromTop (4);

            // --- bottom strip: CHORD row + collapsed RHYTHM / LOOPER -----------
            auto bottom = area.removeFromBottom (86);
            {
                auto chord = bottom.removeFromTop (44);
                auto cbox = section (g, chord, "Chord", tChord);
                selector (g, cbox.removeFromLeft (70).reduced (2), { "OFF", "ON" }, 1); cbox.removeFromLeft (6);
                selector (g, cbox.removeFromLeft (150).reduced (2), { "C","D","E","F","G","A","B" }, 0); cbox.removeFromLeft (6);
                selector (g, cbox.removeFromLeft (150).reduced (2), { "MAJ", "MIN" }, 0); cbox.removeFromLeft (10);
                for (auto* m : { "MAJ","MIN","7TH","DOM7","SUS4","SUS2","DIM" })
                { selector (g, cbox.removeFromLeft (58).reduced (2), { m }, -1); cbox.removeFromLeft (2); }
                bottom.removeFromTop (4);
                auto zones = bottom;
                auto rhythm = zones.removeFromLeft (zones.getWidth() / 2).reduced (0, 0);
                auto looper = zones;
                for (auto z : { std::pair<Rectangle<int>, juce::String> { rhythm, ">  RHYTHM   (arp + 16-step sequencer per part)" },
                                std::pair<Rectangle<int>, juce::String> { looper, ">  LOOPER   (per-part MIDI loops + session export)" } })
                {
                    g.setColour (panelLt()); g.fillRoundedRectangle (z.first.reduced (2, 0).toFloat(), 5.0f);
                    g.setColour (dim()); g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
                    g.drawText (z.second, z.first.reduced (12, 0), juce::Justification::centredLeft, false);
                }
            }
            area.removeFromBottom (4);

            // --- left PART RAIL -------------------------------------------------
            auto rail = area.removeFromLeft (150);
            g.setColour (panelLt()); g.fillRoundedRectangle (rail.toFloat(), 6.0f);
            title (g, rail.reduced (8, 6).removeFromTop (14), "Parts", ink());
            auto rl = rail.reduced (6).withTrimmedTop (20);
            const char* pnames[] { "P1  LIVE  Fat Saw", "P2  808 Basics (kit)", "P3  Warm Pad", "P4  --" };
            for (int i = 0; i < 4; ++i)
            {
                auto cell = rl.removeFromTop (58); rl.removeFromTop (6);
                const bool selp = i == 1;
                g.setColour (selp ? track().brighter (0.15f) : track()); g.fillRoundedRectangle (cell.toFloat(), 5.0f);
                if (selp) { g.setColour (accent()); g.drawRoundedRectangle (cell.toFloat().reduced (1), 5.0f, 2.0f); }
                g.setColour (i == 0 ? accent() : dim()); g.fillEllipse (cell.getX() + 8.0f, cell.getY() + 8.0f, 8.0f, 8.0f);
                g.setColour (ink()); g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
                g.drawText (pnames[i], cell.reduced (22, 4).removeFromTop (16), juce::Justification::centredLeft, true);
                if (selp)   // kit-pad sub-selector seam (P2 is a kit)
                {
                    auto grid = cell.reduced (8).withTrimmedTop (18);
                    for (int p = 0; p < 8; ++p)
                    {
                        auto pad = Rectangle<int> (grid.getX() + (p % 4) * (grid.getWidth() / 4),
                                                   grid.getY() + (p / 4) * (grid.getHeight() / 2),
                                                   grid.getWidth() / 4, grid.getHeight() / 2).reduced (2);
                        g.setColour (p < 6 ? tChord.withAlpha (0.7f) : track()); g.fillRoundedRectangle (pad.toFloat(), 2.0f);
                    }
                }
            }
            area.removeFromLeft (4);

            // --- right SCOPE + FFT ---------------------------------------------
            auto viz = area.removeFromRight (280);
            {
                auto scope = viz.removeFromTop (viz.getHeight() / 2).reduced (0, 0);
                auto sbox = section (g, scope, "Scope  (master)", accent());
                g.setColour (accent()); juce::Path wave; wave.startNewSubPath (sbox.getX(), sbox.getCentreY());
                for (int x = 0; x < sbox.getWidth(); ++x)
                    wave.lineTo (sbox.getX() + x, sbox.getCentreY() - std::sin (x * 0.08f) * std::sin (x * 0.013f) * sbox.getHeight() * 0.4f);
                g.strokePath (wave, juce::PathStrokeType (1.5f));
                viz.removeFromTop (4);
                auto fbox = section (g, viz, "Spectrum  (FFT)", accent());
                juce::Random rng (7);
                const int bars = 40, bw = fbox.getWidth() / bars;
                for (int b = 0; b < bars; ++b)
                {
                    float h = fbox.getHeight() * (0.15f + 0.8f * std::exp (-b * 0.09f) * (0.5f + 0.5f * rng.nextFloat()));
                    g.setColour (accent().withAlpha (0.8f));
                    g.fillRect (Rectangle<float> (fbox.getX() + b * bw + 1.0f, fbox.getBottom() - h, bw - 2.0f, h));
                }
            }
            area.removeFromRight (4);

            // --- centre panel, signal-flow order -------------------------------
            auto centre = area;
            auto col = [&] (int w) { auto c = centre.removeFromLeft (w); centre.removeFromLeft (4); return c; };
            const int u = juce::jmax (60, centre.getWidth() / 12);   // reflow unit

            { auto s = section (g, col (u * 3), "Oscillators", tOsc);
              for (int i = 0; i < 3; ++i) { auto r = s.removeFromLeft (s.getWidth() / (3 - i)); selector (g, r.removeFromTop (24), { "SAW","SQR","TRI","SIN","WT" }, i % 5);
                knob (g, r.removeFromTop (56), "OCT"); knob (g, r, "DET"); } }
            { auto s = section (g, col (u * 1), "Mix", tOsc);
              fader (g, s.removeFromLeft (s.getWidth()/3), "O1", 0.8f); fader (g, s.removeFromLeft (s.getWidth()/2), "O2", 0.6f); fader (g, s, "NZ", 0.2f); }
            { auto s = section (g, col (u * 2), "Filter", tFilt);
              selector (g, s.removeFromTop (24), { "LP","HP","BP","NOTCH" }, 0);
              knob (g, s.removeFromLeft (s.getWidth()/3), "CUT"); knob (g, s.removeFromLeft (s.getWidth()/2), "RES"); knob (g, s, "DRIVE"); }
            { auto s = section (g, col (u * 2), "Amp / Mod Env", tEnv);
              for (auto* l : { "A","D","S","R" }) fader (g, s.removeFromLeft (s.getWidth() / (l[0]=='R'?1: (l[0]=='A'?4:(l[0]=='D'?3:2)))), l, 0.5f); }
            { auto s = section (g, col (u * 2), "LFO 1-3", tLfo);
              for (int i = 0; i < 3; ++i) { auto r = s.removeFromTop (s.getHeight()/(3-i)); knob (g, r.removeFromLeft (r.getWidth()/2), "RATE"); selector (g, r.reduced(2), { "PCH","CUT","PW","OFF" }, 3); } }
            { auto s = section (g, centre, "FX  (drag to reorder)", tFx);
              for (auto* l : { "CHOR","DELAY","REVERB","WIDTH","EQ" }) { auto r = s.removeFromLeft (juce::jmax (46, s.getWidth()/5)); knob (g, r.removeFromTop (r.getHeight()-16), l); } }
        }
    };

    void render (int w, int h, const juce::String& name)
    {
        LayoutMockup m; m.setSize (w, h);
        auto img = m.createComponentSnapshot (m.getLocalBounds(), false, 1.0f);
        REQUIRE (img.isValid());
        juce::File out (juce::String (VASYNTH_DOCS_DIR) + "/" + name);
        out.deleteFile();
        juce::FileOutputStream os (out); REQUIRE (os.openedOk());
        juce::PNGImageFormat png; REQUIRE (png.writeImageToStream (img, os));
    }
}

TEST_CASE ("R2 layout mockup renders at default / fullscreen / narrow", "[plugin][mockup][screenshot]")
{
    juce::ScopedJuceInitialiser_GUI init;
    render (1500, 820, "mockup-default.png");
    render (1920, 1080, "mockup-fullscreen.png");
    render (1120, 660, "mockup-narrow.png");
}
