// ============================================================================
// R2 layout MOCKUP (non-functional). Renders the proposed 1.0 layout so the user
// can sign off on the arrangement BEFORE any attachments are wired. Revision 2:
// bigger controls (arm's-length legible, >=56 px touch), sections FILLED top-to-
// bottom (no dead space), and clear section delineation (filled tinted header bars
// + inset bodies + gaps). Arrangement: slim top bar; left PART RAIL (P1-P4 +
// per-part level + kit-pad sub-selector seam); centre in signal-flow order
// (OSC -> FILTER -> ENV -> LFO -> FX) with osc levels folded into each osc row;
// right SCOPE + FFT of the master; bottom horizontal CHORD row + collapsed
// RHYTHM / LOOPER zones. Rendered to PNGs at default/fullscreen/narrow.
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
    Colour ink_on_tint () { return Colour (0xff0e1319); }

    void ctlLabel (Graphics& g, Rectangle<int> r, const juce::String& t)
    {
        g.setColour (dim()); g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
        g.drawText (t, r, juce::Justification::centred, false);
    }

    // A section: a distinct panel with a FILLED tinted header bar (clear delineation)
    // and an inset body. Returns the padded body rect to fill with controls.
    Rectangle<int> section (Graphics& g, Rectangle<int> r, const juce::String& t, Colour tint)
    {
        g.setColour (panelLt().darker (0.15f)); g.fillRoundedRectangle (r.toFloat(), 7.0f);
        auto head = r.removeFromTop (26);
        g.setColour (tint); g.fillRoundedRectangle (head.toFloat(), 7.0f);
        g.fillRect (head.withTop (head.getCentreY()));            // square off the header's bottom corners
        g.setColour (ink_on_tint()); g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
        g.drawText ("  " + t.toUpperCase(), head, juce::Justification::centredLeft, false);
        g.setColour (panelLt()); g.fillRect (r.reduced (3, 0).withTrimmedBottom (3));
        return r.reduced (8, 6);
    }

    // Big knob (fills its cell up to ~72 px) + a value tick ring + label under.
    void knob (Graphics& g, Rectangle<int> r, const juce::String& label, float v = 0.62f)
    {
        auto lab = r.removeFromBottom (14);
        const int d = juce::jmin (juce::jmin (r.getWidth() - 6, r.getHeight() - 6), 104);
        auto c = r.withSizeKeepingCentre (d, d).toFloat();
        g.setColour (track()); g.fillEllipse (c);
        g.setColour (track().brighter (0.2f)); g.drawEllipse (c.reduced (1.0f), 1.0f);
        // value arc
        const auto ctr = c.getCentre(); const float r0 = c.getWidth() * 0.5f - 3.0f;
        const float a0 = 2.35f, a1 = a0 + v * (2.0f * 3.14159f - 2.0f * 0.8f);
        juce::Path arc; arc.addCentredArc (ctr.x, ctr.y, r0, r0, 0.0f, a0, a1, true);
        g.setColour (accent()); g.strokePath (arc, juce::PathStrokeType (3.0f));
        g.setColour (ink()); g.drawLine (ctr.x, ctr.y, ctr.x + std::cos (a1 + 1.57f) * r0 * 0.9f,
                                         ctr.y + std::sin (a1 + 1.57f) * r0 * 0.9f, 2.5f);
        ctlLabel (g, lab, label);
    }

    void fader (Graphics& g, Rectangle<int> r, const juce::String& label, float v)
    {
        auto lab = r.removeFromBottom (14);
        auto col = r.withSizeKeepingCentre (juce::jmin (16, r.getWidth() - 4), r.getHeight() - 6);
        g.setColour (track()); g.fillRoundedRectangle (col.toFloat(), 5.0f);
        const int thumbY = col.getY() + (int) ((1.0f - v) * (col.getHeight() - 18));
        g.setColour (accent()); g.fillRoundedRectangle (Rectangle<int> (col.getX() - 5, thumbY, col.getWidth() + 10, 18).toFloat(), 4.0f);
        ctlLabel (g, lab, label);
    }

    // A tall, chunky one-tap segmented selector (>=56 px cells where width allows).
    void selector (Graphics& g, Rectangle<int> r, juce::StringArray opts, int sel, bool vertical = false)
    {
        const int n = juce::jmax (1, opts.size());
        for (int i = 0; i < opts.size(); ++i)
        {
            auto cell = vertical ? Rectangle<int> (r.getX(), r.getY() + i * r.getHeight() / n, r.getWidth(), r.getHeight() / n).reduced (2)
                                 : Rectangle<int> (r.getX() + i * r.getWidth() / n, r.getY(), r.getWidth() / n, r.getHeight()).reduced (2);
            g.setColour (i == sel ? accent() : track()); g.fillRoundedRectangle (cell.toFloat(), 4.0f);
            g.setColour (i == sel ? ink_on_tint() : ink()); g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
            g.drawText (opts[i], cell, juce::Justification::centred, false);
        }
    }

    Rectangle<int> takeL (Rectangle<int>& r, int w) { auto c = r.removeFromLeft (w); r.removeFromLeft (6); return c; }
    Rectangle<int> takeT (Rectangle<int>& r, int h) { auto c = r.removeFromTop (h);  r.removeFromTop (6);  return c; }

    struct LayoutMockup : public juce::Component
    {
        void paint (Graphics& g) override
        {
            g.fillAll (panel().darker (0.25f));                 // darker backdrop -> sections pop
            auto area = getLocalBounds().reduced (6);
            const auto tOsc = accent(), tFilt = Colour (0xff6ea8ff), tEnv = Colour (0xffb07cff),
                       tLfo = Colour (0xfff0a04b), tFx = Colour (0xff5ecb8a), tChord = Colour (0xffe0733a),
                       tViz = Colour (0xff67c0c8), tParts = Colour (0xff9aa3ad);

            // --- slim top bar ---------------------------------------------------
            auto top = area.removeFromTop (48);
            g.setColour (panelLt()); g.fillRoundedRectangle (top.toFloat(), 6.0f);
            auto tb = top.reduced (10, 7);
            auto preset = tb.removeFromLeft (260); g.setColour (track()); g.fillRoundedRectangle (preset.toFloat(), 5.0f);
            g.setColour (ink()); g.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
            g.drawText ("  synth        Fat Saw Bass", preset, juce::Justification::centredLeft, false);
            auto help = tb.removeFromRight (36); g.setColour (track()); g.fillRoundedRectangle (help.toFloat(), 5.0f);
            g.setColour (ink()); g.drawText ("?", help, juce::Justification::centred, false);
            tb.removeFromRight (8);
            auto ind = tb.removeFromRight (250);
            g.setColour (dim()); g.setFont (juce::Font (juce::FontOptions (11.5f)));
            g.drawText ("CPU 12%    MIDI ok    CLK 120 int    SCOPE [on]", ind, juce::Justification::centredRight, false);
            auto rec = tb.removeFromRight (78); g.setColour (Colour (0xffd8443a)); g.fillEllipse (rec.removeFromLeft (18).toFloat().reduced (2));
            g.setColour (ink()); g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold))); g.drawText (" REC", rec, juce::Justification::centredLeft, false);
            knob (g, tb.removeFromRight (66), "MASTER", 0.7f);
            area.removeFromTop (6);

            // --- bottom strip: CHORD row + collapsed RHYTHM / LOOPER -----------
            auto bottom = area.removeFromBottom (108);
            {
                auto chord = bottom.removeFromTop (56);
                auto c = section (g, chord, "Chord", tChord);
                selector (g, takeL (c, 96), { "OFF", "ON" }, 1);
                selector (g, takeL (c, 210), { "C","D","E","F","G","A","B" }, 0);
                selector (g, takeL (c, 130), { "MAJ", "MIN" }, 0);
                c.removeFromLeft (6);
                for (auto* m : { "MAJ","MIN","7TH","DOM7","SUS4","SUS2","DIM" }) selector (g, takeL (c, juce::jmax (60, c.getWidth()/8)), { m }, -1);
                bottom.removeFromTop (6);
                auto rhythm = takeL (bottom, bottom.getWidth() / 2 - 3);
                for (auto z : { std::pair<Rectangle<int>, juce::String> { rhythm, ">   RHYTHM      arp + 16-step sequencer per part" },
                                std::pair<Rectangle<int>, juce::String> { bottom, ">   LOOPER      per-part MIDI loops + session export" } })
                {
                    g.setColour (panelLt()); g.fillRoundedRectangle (z.first.toFloat(), 6.0f);
                    g.setColour (ink().withAlpha (0.85f)); g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
                    g.drawText (z.second, z.first.reduced (14, 0), juce::Justification::centredLeft, false);
                }
            }
            area.removeFromBottom (6);

            // --- left PART RAIL (taller cells: name + level fader + kit pads) ---
            auto rail = takeL (area, 178);
            {
                auto rl = section (g, rail, "Parts", tParts);
                const char* pn[]  { "P1  LIVE", "P2  808 Basics", "P3  Warm Pad", "P4  (empty)" };
                const char* sub[] { "Fat Saw Bass", "kit  -  6 pads", "chorus+reverb", "tap to add" };
                for (int i = 0; i < 4; ++i)
                {
                    auto cell = takeT (rl, (rl.getHeight() - 18) / 4);
                    const bool selp = i == 1;
                    g.setColour (selp ? track().brighter (0.16f) : track()); g.fillRoundedRectangle (cell.toFloat(), 6.0f);
                    if (selp) { g.setColour (accent()); g.drawRoundedRectangle (cell.toFloat().reduced (1), 6.0f, 2.0f); }
                    auto lvl = cell.removeFromRight (26).reduced (4, 8);   // per-part level
                    g.setColour (track().darker (0.3f)); g.fillRoundedRectangle (lvl.toFloat(), 3.0f);
                    g.setColour (accent()); g.fillRoundedRectangle (lvl.withTrimmedTop (lvl.getHeight() / 3).toFloat(), 3.0f);
                    auto body = cell.reduced (8, 5);
                    g.setColour (i == 0 ? accent() : dim()); g.fillEllipse (body.removeFromLeft (12).removeFromTop (12).toFloat());
                    g.setColour (ink()); g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
                    g.drawText (pn[i], body.removeFromTop (17).withTrimmedLeft (4), juce::Justification::centredLeft, false);
                    g.setColour (dim()); g.setFont (juce::Font (juce::FontOptions (10.5f)));
                    g.drawText (sub[i], body.removeFromTop (14).withTrimmedLeft (4), juce::Justification::centredLeft, false);
                    if (selp)   // kit-pad sub-selector seam (4x2 pads)
                    {
                        auto grid = body.withTrimmedTop (2);
                        for (int p = 0; p < 8; ++p)
                        {
                            auto pad = Rectangle<int> (grid.getX() + 4 + (p % 4) * (grid.getWidth() / 4),
                                                       grid.getY() + (p / 4) * (grid.getHeight() / 2),
                                                       grid.getWidth() / 4, grid.getHeight() / 2).reduced (2);
                            g.setColour (p < 6 ? tChord.withAlpha (0.75f) : track().darker (0.2f)); g.fillRoundedRectangle (pad.toFloat(), 3.0f);
                        }
                    }
                }
            }

            // --- right SCOPE + FFT ---------------------------------------------
            auto viz = area.removeFromRight (300); area.removeFromRight (6);
            {
                auto scope = viz.removeFromTop (viz.getHeight() / 2); viz.removeFromTop (6);
                auto sb = section (g, scope, "Scope  -  master", tViz);
                g.setColour (accent()); juce::Path w; w.startNewSubPath ((float) sb.getX(), (float) sb.getCentreY());
                for (int x = 0; x < sb.getWidth(); ++x)
                    w.lineTo ((float) (sb.getX() + x), sb.getCentreY() - std::sin (x * 0.075f) * std::sin (x * 0.012f) * sb.getHeight() * 0.42f);
                g.strokePath (w, juce::PathStrokeType (2.0f));
                auto fb = section (g, viz, "Spectrum  -  FFT", tViz);
                juce::Random rng (7); const int bars = 44, bw = fb.getWidth() / bars;
                for (int b = 0; b < bars; ++b)
                { float h = fb.getHeight() * (0.12f + 0.85f * std::exp (-b * 0.085f) * (0.5f + 0.5f * rng.nextFloat()));
                  g.setColour (accent().withAlpha (0.85f)); g.fillRect (Rectangle<float> ((float) (fb.getX() + b * bw + 1), fb.getBottom() - h, bw - 2.0f, h)); }
            }

            // --- centre panel, signal-flow order, sections FILLED --------------
            auto centre = area;
            const int u = juce::jmax (74, (centre.getWidth() - 5 * 6) / 13);

            // OSCILLATORS: three osc rows, each Wave selector + Oct/Det/PW knobs + Level fader.
            { auto s = section (g, takeL (centre, u * 4), "Oscillators + Mix", tOsc);
              for (int i = 0; i < 3; ++i)
              { auto row = takeT (s, (s.getHeight() - 12) / 3);
                selector (g, takeL (row, 130), { "SAW","SQR","TRI","SIN","WT" }, i);
                knob (g, takeL (row, row.getWidth()/4), "OCT"); knob (g, takeL (row, row.getWidth()/3), "DET");
                knob (g, takeL (row, row.getWidth()/2), "PW"); fader (g, row, "LVL", 0.8f - i * 0.2f); } }

            // FILTER: type selector, then two filled rows (Cut/Res, then Drive/Env/Track).
            { auto s = section (g, takeL (centre, u * 2), "Filter", tFilt);
              selector (g, takeT (s, 42), { "LP","HP","BP","NOTCH" }, 0);
              auto r1 = takeT (s, s.getHeight() / 2);
              knob (g, takeL (r1, r1.getWidth()/2), "CUTOFF", 0.7f); knob (g, r1, "RESO", 0.35f);
              knob (g, takeL (s, s.getWidth()/3), "DRIVE", 0.2f); knob (g, takeL (s, s.getWidth()/2), "ENV"); knob (g, s, "TRACK"); }

            // ENV: Amp/Mod tab, then four big ADSR faders + env->pitch knob.
            { auto s = section (g, takeL (centre, u * 2), "Envelope", tEnv);
              selector (g, takeT (s, 34), { "AMP","MOD" }, 0);
              auto k = s.removeFromRight (s.getWidth()/5);
              for (auto* l : { "A","D","S","R" }) fader (g, takeL (s, s.getWidth()/(5 - (l[0]=='A'?0:l[0]=='D'?1:l[0]=='S'?2:3)) ), l, 0.55f);
              knob (g, k, "ENV>PCH"); }

            // LFO 1-3: three rows, each Rate + Depth knobs + Shape + Dest selectors.
            { auto s = section (g, takeL (centre, u * 3), "LFO 1 . 2 . 3", tLfo);
              for (int i = 0; i < 3; ++i)
              { auto row = takeT (s, (s.getHeight() - 12) / 3);
                knob (g, takeL (row, row.getWidth()/4), "RATE"); knob (g, takeL (row, row.getWidth()/3), "DEPTH");
                selector (g, takeL (row, row.getWidth()/2), { "TRI","SIN","SQR","S&H" }, 1);
                selector (g, row, { "PCH","CUT","PW","OFF" }, i); } }

            // FX: five stacked blocks, each on/off + name + mix knob (drag to reorder).
            { auto s = section (g, centre, "FX  -  drag to reorder", tFx);
              const char* fx[] { "CHORUS","DELAY","REVERB","WIDTH","EQ" };
              for (int i = 0; i < 5; ++i)
              { auto row = takeT (s, (s.getHeight() - 24) / 5);
                g.setColour (track()); g.fillRoundedRectangle (row.toFloat(), 5.0f);
                auto tog = row.removeFromLeft (30).reduced (6); g.setColour (i < 3 ? accent() : dim()); g.fillRoundedRectangle (tog.toFloat(), 3.0f);
                g.setColour (ink()); g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
                g.drawText (fx[i], row.removeFromLeft (row.getWidth() - 56).withTrimmedLeft (6), juce::Justification::centredLeft, false);
                knob (g, row, "MIX", 0.4f); } }
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
    render (1500, 860, "mockup-default.png");
    render (1920, 1080, "mockup-fullscreen.png");
    render (1120, 700, "mockup-narrow.png");
}
