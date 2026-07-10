// ============================================================================
// R2 layout MOCKUP (non-functional) - revision 3. Direction from sign-off:
// denser (more params per section) + creative + more COMPACT synth, freeing real
// estate for the RHYTHM (arp + 16-step sequencer) and LOOPER (per-part loop lanes)
// zones. Filled tinted section headers kept. Arrangement: slim top bar; left PART
// RAIL (P1-P4 + level + kit-pad seam); compact centre in signal-flow order with
// MORE params visible; right SCOPE + FFT; bottom = CHORD row + substantial RHYTHM
// + LOOPER zones (shown expanded here to prove the space; collapsed by default at
// runtime, synth reflows into the freed space). Rendered at default/fullscreen/narrow.
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
    Colour onTint  () { return Colour (0xff0e1319); }

    Rectangle<int> takeL (Rectangle<int>& r, int w) { auto c = r.removeFromLeft (w); r.removeFromLeft (5); return c; }
    Rectangle<int> takeT (Rectangle<int>& r, int h) { auto c = r.removeFromTop (h);  r.removeFromTop (5);  return c; }

    void clabel (Graphics& g, Rectangle<int> r, const juce::String& t)
    { g.setColour (dim()); g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold))); g.drawText (t, r, juce::Justification::centred, false); }

    Rectangle<int> section (Graphics& g, Rectangle<int> r, const juce::String& t, Colour tint)
    {
        g.setColour (panelLt().darker (0.12f)); g.fillRoundedRectangle (r.toFloat(), 6.0f);
        auto head = r.removeFromTop (24);
        g.setColour (tint); g.fillRoundedRectangle (head.toFloat(), 6.0f); g.fillRect (head.withTop (head.getCentreY()));
        g.setColour (onTint()); g.setFont (juce::Font (juce::FontOptions (12.5f, juce::Font::bold)));
        g.drawText ("  " + t.toUpperCase(), head, juce::Justification::centredLeft, false);
        return r.reduced (6, 5);
    }

    void knob (Graphics& g, Rectangle<int> r, const juce::String& label, float v = 0.62f)
    {
        auto lab = r.removeFromBottom (13);
        const int d = juce::jmin (juce::jmin (r.getWidth() - 2, r.getHeight() - 2), 96);
        auto c = r.withSizeKeepingCentre (d, d).toFloat();
        g.setColour (track()); g.fillEllipse (c);
        const auto ctr = c.getCentre(); const float r0 = c.getWidth() * 0.5f - 3.0f;
        const float a0 = 2.30f, a1 = a0 + v * 4.66f;
        juce::Path arc; arc.addCentredArc (ctr.x, ctr.y, r0, r0, 0.0f, a0, a1, true);
        g.setColour (accent()); g.strokePath (arc, juce::PathStrokeType (3.0f));
        g.setColour (ink()); g.drawLine (ctr.x, ctr.y, ctr.x + std::cos (a1 + 1.57f) * r0 * 0.85f, ctr.y + std::sin (a1 + 1.57f) * r0 * 0.85f, 2.2f);
        clabel (g, lab, label);
    }

    void fader (Graphics& g, Rectangle<int> r, const juce::String& label, float v)
    {
        auto lab = r.removeFromBottom (13);
        auto col = r.withSizeKeepingCentre (juce::jmin (15, r.getWidth() - 2), r.getHeight() - 4);
        g.setColour (track()); g.fillRoundedRectangle (col.toFloat(), 4.0f);
        const int ty = col.getY() + (int) ((1.0f - v) * (col.getHeight() - 16));
        g.setColour (accent()); g.fillRoundedRectangle (Rectangle<int> (col.getX() - 4, ty, col.getWidth() + 8, 16).toFloat(), 3.0f);
        clabel (g, lab, label);
    }

    void selector (Graphics& g, Rectangle<int> r, juce::StringArray opts, int sel)
    {
        const int n = juce::jmax (1, opts.size());
        for (int i = 0; i < opts.size(); ++i)
        {
            auto cell = Rectangle<int> (r.getX() + i * r.getWidth() / n, r.getY(), r.getWidth() / n, r.getHeight()).reduced (2);
            g.setColour (i == sel ? accent() : track()); g.fillRoundedRectangle (cell.toFloat(), 4.0f);
            g.setColour (i == sel ? onTint() : ink()); g.setFont (juce::Font (juce::FontOptions (11.5f, juce::Font::bold)));
            g.drawText (opts[i], cell, juce::Justification::centred, false);
        }
    }
    void toggle (Graphics& g, Rectangle<int> r, const juce::String& t, bool on)
    {
        g.setColour (on ? accent() : track()); g.fillRoundedRectangle (r.toFloat(), 4.0f);
        g.setColour (on ? onTint() : ink()); g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
        g.drawText (t, r, juce::Justification::centred, false);
    }

    struct LayoutMockup : public juce::Component
    {
        void paint (Graphics& g) override
        {
            g.fillAll (panel().darker (0.25f));
            auto area = getLocalBounds().reduced (6);
            const auto tOsc = accent(), tFilt = Colour (0xff6ea8ff), tEnv = Colour (0xffb07cff),
                       tLfo = Colour (0xfff0a04b), tFx = Colour (0xff5ecb8a), tChord = Colour (0xffe0733a),
                       tViz = Colour (0xff67c0c8), tParts = Colour (0xff9aa3ad), tRhy = Colour (0xffe0b13a), tLoop = Colour (0xffca6bd0);

            // --- slim top bar ---------------------------------------------------
            auto top = takeT (area, 46);
            g.setColour (panelLt()); g.fillRoundedRectangle (top.toFloat(), 6.0f);
            auto tb = top.reduced (10, 7);
            auto preset = takeL (tb, 250); g.setColour (track()); g.fillRoundedRectangle (preset.toFloat(), 5.0f);
            g.setColour (ink()); g.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
            g.drawText ("  synth        Fat Saw Bass", preset, juce::Justification::centredLeft, false);
            auto help = tb.removeFromRight (34); g.setColour (track()); g.fillRoundedRectangle (help.toFloat(), 5.0f);
            g.setColour (ink()); g.drawText ("?", help, juce::Justification::centred, false); tb.removeFromRight (8);
            g.setColour (dim()); g.setFont (juce::Font (juce::FontOptions (11.5f)));
            g.drawText ("CPU 12%   MIDI ok   CLK 120 int   SCOPE [on]", tb.removeFromRight (250), juce::Justification::centredRight, false);
            auto rec = tb.removeFromRight (74); g.setColour (Colour (0xffd8443a)); g.fillEllipse (rec.removeFromLeft (16).toFloat().reduced (2));
            g.setColour (ink()); g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold))); g.drawText (" REC", rec, juce::Justification::centredLeft, false);
            knob (g, tb.removeFromRight (60), "MASTER", 0.7f);

            // --- BOTTOM WORKSTATION: chord + RHYTHM + LOOPER (substantial) ------
            auto workH = juce::jmax (250, area.getHeight() * 38 / 100);
            auto work = area.removeFromBottom (workH);
            {
                auto chord = takeT (work, 48);
                auto c = section (g, chord, "Chord", tChord);
                selector (g, takeL (c, 92), { "OFF", "ON" }, 1);
                selector (g, takeL (c, 200), { "C","D","E","F","G","A","B" }, 0);
                selector (g, takeL (c, 120), { "MAJ", "MIN" }, 0); c.removeFromLeft (4);
                for (auto* m : { "MAJ","MIN","7TH","DOM7","SUS4","SUS2","DIM" }) selector (g, takeL (c, juce::jmax (56, c.getWidth()/8)), { m }, -1);

                auto rhythm = takeL (work, work.getWidth() * 55 / 100);
                auto rb = section (g, rhythm, "Rhythm  -  arp + sequencer  (part 2)", tRhy);
                { auto ctrls = takeT (rb, 44);
                  selector (g, takeL (ctrls, 250), { "UP","DOWN","UP/DN","RAND","PLAYED" }, 0);
                  knob (g, takeL (ctrls, 60), "OCT"); knob (g, takeL (ctrls, 60), "GATE"); knob (g, takeL (ctrls, 60), "SWING");
                  toggle (g, takeL (ctrls, 66), "LATCH", true); toggle (g, ctrls.removeFromLeft (66), "HOLD", false); }
                { auto lane = rb;                                   // 16-step grid
                  const int cells = 16; const int cw = lane.getWidth() / cells;
                  for (int s = 0; s < cells; ++s)
                  { auto cell = Rectangle<int> (lane.getX() + s * cw, lane.getY(), cw, lane.getHeight()).reduced (2);
                    const bool on = (s % 4 == 0) || s == 6 || s == 10 || s == 13;
                    g.setColour (on ? tRhy : track()); g.fillRoundedRectangle (cell.toFloat(), 3.0f);
                    if (on) { g.setColour (onTint().withAlpha (0.4f)); g.fillRect (cell.removeFromBottom (cell.getHeight() * (s % 3 + 1) / 4)); } } }

                auto lb = section (g, work, "Looper  -  per-part MIDI loops + session export", tLoop);
                { auto bar = takeT (lb, 40);
                  toggle (g, takeL (bar, 70), "REC", false); toggle (g, takeL (bar, 70), "PLAY", true);
                  toggle (g, takeL (bar, 70), "CLEAR", false); toggle (g, takeL (bar, 90), "SYNC 1 bar", true);
                  g.setColour (dim()); g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
                  g.drawText ("EXPORT SESSION  ->  stems + MIDI", bar, juce::Justification::centredRight, false); }
                const char* lanes[] { "P1 lead", "P2 drums", "P3 pad", "P4 --" };
                for (int i = 0; i < 4; ++i)
                { auto lane = takeT (lb, (lb.getHeight() - 3 * 5) / 4);
                  g.setColour (track()); g.fillRoundedRectangle (lane.toFloat(), 4.0f);
                  g.setColour (ink()); g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
                  g.drawText (lanes[i], lane.removeFromLeft (70).withTrimmedLeft (8), juce::Justification::centredLeft, false);
                  if (i < 3) { g.setColour (tLoop.withAlpha (0.5f)); juce::Random rr (i + 3);
                    for (int x = 0; x < lane.getWidth(); x += 6) { float h = lane.getHeight() * (0.2f + 0.7f * rr.nextFloat());
                      g.fillRect (Rectangle<float> ((float) (lane.getX() + x), lane.getCentreY() - h/2, 3.0f, h)); } } }
            }

            // --- left PART RAIL -------------------------------------------------
            auto rail = takeL (area, 176);
            {
                auto rl = section (g, rail, "Parts", tParts);
                const char* pn[]  { "P1  LIVE", "P2  808 Basics", "P3  Warm Pad", "P4  (empty)" };
                const char* sub[] { "Fat Saw Bass", "kit  -  6 pads", "chorus + reverb", "tap to add" };
                for (int i = 0; i < 4; ++i)
                {
                    auto cell = takeT (rl, (rl.getHeight() - 15) / 4);
                    const bool selp = i == 1;
                    g.setColour (selp ? track().brighter (0.16f) : track()); g.fillRoundedRectangle (cell.toFloat(), 6.0f);
                    if (selp) { g.setColour (accent()); g.drawRoundedRectangle (cell.toFloat().reduced (1), 6.0f, 2.0f); }
                    auto lvl = cell.removeFromRight (24).reduced (4, 7);
                    g.setColour (track().darker (0.35f)); g.fillRoundedRectangle (lvl.toFloat(), 3.0f);
                    g.setColour (accent()); g.fillRoundedRectangle (lvl.withTrimmedTop (lvl.getHeight()/3).toFloat(), 3.0f);
                    auto body = cell.reduced (7, 5);
                    g.setColour (i == 0 ? accent() : dim()); g.fillEllipse (body.removeFromLeft (11).removeFromTop (11).toFloat());
                    g.setColour (ink()); g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
                    g.drawText (pn[i], body.removeFromTop (16).withTrimmedLeft (4), juce::Justification::centredLeft, false);
                    g.setColour (dim()); g.setFont (juce::Font (juce::FontOptions (10.0f)));
                    g.drawText (sub[i], body.removeFromTop (13).withTrimmedLeft (4), juce::Justification::centredLeft, false);
                    if (selp) { auto grid = body.withTrimmedTop (2);
                      for (int p = 0; p < 8; ++p) { auto pad = Rectangle<int> (grid.getX() + 3 + (p%4)*(grid.getWidth()/4), grid.getY() + (p/4)*(grid.getHeight()/2), grid.getWidth()/4, grid.getHeight()/2).reduced (2);
                        g.setColour (p < 6 ? tChord.withAlpha (0.75f) : track().darker (0.2f)); g.fillRoundedRectangle (pad.toFloat(), 3.0f); } }
                }
            }

            // --- right SCOPE + FFT ---------------------------------------------
            auto viz = area.removeFromRight (280); area.removeFromRight (5);
            {
                auto sb = section (g, takeT (viz, viz.getHeight() / 2), "Scope  -  master", tViz);
                g.setColour (accent()); juce::Path w; w.startNewSubPath ((float) sb.getX(), (float) sb.getCentreY());
                for (int x = 0; x < sb.getWidth(); ++x) w.lineTo ((float)(sb.getX()+x), sb.getCentreY() - std::sin (x*0.075f)*std::sin (x*0.012f)*sb.getHeight()*0.42f);
                g.strokePath (w, juce::PathStrokeType (2.0f));
                auto fb = section (g, viz, "Spectrum  -  FFT", tViz);
                juce::Random rng (7); const int bars = 42, bw = fb.getWidth()/bars;
                for (int b = 0; b < bars; ++b) { float h = fb.getHeight()*(0.12f+0.85f*std::exp (-b*0.085f)*(0.5f+0.5f*rng.nextFloat()));
                  g.setColour (accent().withAlpha (0.85f)); g.fillRect (Rectangle<float> ((float)(fb.getX()+b*bw+1), fb.getBottom()-h, bw-2.0f, h)); }
            }

            // --- centre synth, COMPACT + dense (more params) --------------------
            auto centre = area;
            const int u = juce::jmax (78, (centre.getWidth() - 4 * 5) / 13);

            { auto s = section (g, takeL (centre, u * 4), "Oscillators + Mix", tOsc);   // 3 rows x 6 params
              for (int i = 0; i < 3; ++i) { auto row = takeT (s, (s.getHeight() - 10) / 3);
                selector (g, takeL (row, 116), { "SAW","SQR","TRI","SIN","WT" }, i);
                for (auto* l : { "OCT","DET","PW","FINE" }) knob (g, takeL (row, row.getWidth()/(l[0]=='O'?4:l[0]=='D'?3:l[0]=='P'?2:1)), l);
                fader (g, row.removeFromLeft (0).withWidth (0), "", 0.5f); } }

            { auto s = section (g, takeL (centre, u * 2), "Filter", tFilt);
              selector (g, takeT (s, 34), { "LP","HP","BP","NOTCH" }, 0);
              auto r1 = takeT (s, s.getHeight() * 3 / 5);
              knob (g, takeL (r1, r1.getWidth()/2), "CUTOFF", 0.7f); knob (g, r1, "RESO", 0.35f);
              for (auto* l : { "DRIVE","ENV","TRACK" }) knob (g, takeL (s, s.getWidth()/(l[0]=='D'?3:l[0]=='E'?2:1)), l, 0.2f); }

            { auto s = section (g, takeL (centre, u * 2), "Envelope", tEnv);
              selector (g, takeT (s, 30), { "AMP","MOD" }, 0);
              auto kk = s.removeFromRight (s.getWidth() * 2 / 6);
              for (auto* l : { "A","D","S","R" }) fader (g, takeL (s, s.getWidth()/(l[0]=='A'?4:l[0]=='D'?3:l[0]=='S'?2:1)), l, 0.55f);
              knob (g, takeT (kk, kk.getHeight()/2), "E>PCH"); knob (g, kk, "VEL"); }

            { auto s = section (g, takeL (centre, u * 3), "LFO 1 . 2 . 3", tLfo);        // 3 rows x 4 params
              for (int i = 0; i < 3; ++i) { auto row = takeT (s, (s.getHeight() - 10) / 3);
                knob (g, takeL (row, row.getWidth()/4), "RATE"); knob (g, takeL (row, row.getWidth()/3), "DEP");
                selector (g, takeL (row, row.getWidth()/2), { "TRI","SIN","SQR","S&H" }, 1);
                selector (g, row, { "PCH","CUT","PW","OFF" }, i); } }

            { auto s = section (g, centre, "FX  -  drag to reorder", tFx);              // 5 blocks + on/off
              const char* fx[] { "CHORUS","DELAY","REVERB","WIDTH","EQ" };
              for (int i = 0; i < 5; ++i) { auto row = takeT (s, (s.getHeight() - 20) / 5);
                g.setColour (track()); g.fillRoundedRectangle (row.toFloat(), 5.0f);
                auto t = row.removeFromLeft (28).reduced (5); g.setColour (i < 3 ? accent() : dim()); g.fillRoundedRectangle (t.toFloat(), 3.0f);
                g.setColour (ink()); g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
                g.drawText (fx[i], row.removeFromLeft (row.getWidth() - 52).withTrimmedLeft (6), juce::Justification::centredLeft, false);
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
    render (1500, 900, "mockup-default.png");
    render (1920, 1080, "mockup-fullscreen.png");
    render (1180, 760, "mockup-narrow.png");
}
