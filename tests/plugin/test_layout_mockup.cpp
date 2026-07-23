// ============================================================================
// R2 layout MOCKUP (non-functional) - revision 4, per detailed sign-off notes:
//  - Parts rail: even-height cells (fixed a shrinking-row bug).
//  - Oscillators: subdivided into a box PER OSC; wave shapes as touch buttons across
//    the top (last = WT, opens a wavetable picker); OCT/DET/PW/FINE knobs below.
//  - Filter: type buttons 50% taller; all knobs stacked VERTICALLY at equal size.
//  - Envelope: unchanged (AMP/MOD + ADSR + Env>Pitch/Vel).
//  - LFO: subdivided into a box PER LFO (like oscillators); DEST selector horizontal
//    across the top with full labels; Rate/Depth knobs + Shape below.
//  - FX: reverted to the real draggable panel - Chorus/Delay/Reverb/Width blocks with
//    their own knobs + NUMERIC values, recoloured to match the UI.
//  - Chord row, Scope/FFT, RHYTHM sequencer, LOOPER lanes: as before.
// Rendered at default / fullscreen / narrow.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include "UI/VASynthLookAndFeel.h"
#include <cmath>
#include <vector>

#ifndef VASYNTH_DOCS_DIR
 #define VASYNTH_DOCS_DIR "."
#endif

namespace
{
    using juce::Rectangle; using juce::Colour; using juce::Graphics; using juce::String;

    Colour panel   () { return VASynthLookAndFeel::panel(); }
    Colour panelLt () { return VASynthLookAndFeel::panelLight(); }
    Colour ink     () { return VASynthLookAndFeel::ink(); }
    Colour dim     () { return VASynthLookAndFeel::dim(); }
    Colour accent  () { return VASynthLookAndFeel::accent(); }
    Colour track   () { return VASynthLookAndFeel::track(); }
    Colour onTint  () { return Colour (0xff0e1319); }

    Rectangle<int> takeL (Rectangle<int>& r, int w) { auto c = r.removeFromLeft (w); r.removeFromLeft (5); return c; }
    Rectangle<int> takeT (Rectangle<int>& r, int h) { auto c = r.removeFromTop (h);  r.removeFromTop (5);  return c; }

    void clabel (Graphics& g, Rectangle<int> r, const String& t)
    { g.setColour (dim()); g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold))); g.drawText (t, r, juce::Justification::centred, false); }

    Rectangle<int> section (Graphics& g, Rectangle<int> r, const String& t, Colour tint)
    {
        g.setColour (panelLt().darker (0.12f)); g.fillRoundedRectangle (r.toFloat(), 6.0f);
        auto head = r.removeFromTop (24);
        g.setColour (tint); g.fillRoundedRectangle (head.toFloat(), 6.0f); g.fillRect (head.withTop (head.getCentreY()));
        g.setColour (onTint()); g.setFont (juce::Font (juce::FontOptions (12.5f, juce::Font::bold)));
        g.drawText ("  " + t.toUpperCase(), head, juce::Justification::centredLeft, false);
        return r.reduced (6, 5);
    }

    void knobAt (Graphics& g, Rectangle<int> r, float v, int maxD)
    {
        const int d = juce::jmin (juce::jmin (r.getWidth() - 2, r.getHeight() - 2), maxD);
        auto c = r.withSizeKeepingCentre (d, d).toFloat();
        g.setColour (track()); g.fillEllipse (c);
        const auto ctr = c.getCentre(); const float r0 = c.getWidth() * 0.5f - 3.0f;
        const float a0 = 2.30f, a1 = a0 + v * 4.66f;
        juce::Path arc; arc.addCentredArc (ctr.x, ctr.y, r0, r0, 0.0f, a0, a1, true);
        g.setColour (accent()); g.strokePath (arc, juce::PathStrokeType (3.0f));
        g.setColour (ink()); g.drawLine (ctr.x, ctr.y, ctr.x + std::cos (a1 + 1.57f) * r0 * 0.85f, ctr.y + std::sin (a1 + 1.57f) * r0 * 0.85f, 2.2f);
    }
    void knob (Graphics& g, Rectangle<int> r, const String& label, float v = 0.62f)
    { auto lab = r.removeFromBottom (13); knobAt (g, r, v, 92); clabel (g, lab, label); }

    // knob + label + numeric value (FX controls).
    void knobV (Graphics& g, Rectangle<int> r, const String& label, const String& val, float v)
    {
        auto valR = r.removeFromBottom (13); auto lab = r.removeFromBottom (12);
        knobAt (g, r, v, 58);
        clabel (g, lab, label);
        g.setColour (accent()); g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 10.5f, juce::Font::bold)));
        g.drawText (val, valR, juce::Justification::centred, false);
    }

    void fader (Graphics& g, Rectangle<int> r, const String& label, float v)
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
            const bool wt = opts[i].startsWith ("WT");
            g.setColour (i == sel ? accent() : (wt ? track().brighter (0.12f) : track())); g.fillRoundedRectangle (cell.toFloat(), 4.0f);
            g.setColour (i == sel ? onTint() : ink()); g.setFont (juce::Font (juce::FontOptions (11.5f, juce::Font::bold)));
            g.drawText (opts[i], cell, juce::Justification::centred, false);
        }
    }
    void toggle (Graphics& g, Rectangle<int> r, const String& t, bool on)
    {
        g.setColour (on ? accent() : track()); g.fillRoundedRectangle (r.toFloat(), 4.0f);
        g.setColour (on ? onTint() : ink()); g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
        g.drawText (t, r, juce::Justification::centred, false);
    }
    void grip (Graphics& g, Rectangle<int> r)   // drag handle (two columns of dots)
    {
        g.setColour (dim());
        for (int x = 0; x < 2; ++x) for (int y = 0; y < 3; ++y)
            g.fillEllipse ((float) (r.getCentreX() - 3 + x * 4), (float) (r.getCentreY() - 6 + y * 5), 2.4f, 2.4f);
    }
    Rectangle<int> subBox (Graphics& g, Rectangle<int> r, Colour tint)   // an inset box for one osc / lfo
    {
        g.setColour (panel()); g.fillRoundedRectangle (r.toFloat(), 5.0f);
        g.setColour (tint.withAlpha (0.4f)); g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 5.0f, 1.0f);
        return r.reduced (6, 5);
    }

    // A little waveform icon button (LFO shape): 0 Tri, 1 Sin, 2 Sqr, 3 S&H.
    void shapeIcon (Graphics& g, Rectangle<int> r, int kind, bool on)
    {
        g.setColour (on ? accent() : track()); g.fillRoundedRectangle (r.toFloat(), 4.0f);
        auto a = r.toFloat().reduced (r.getWidth() * 0.18f, r.getHeight() * 0.28f);
        const float x0 = a.getX(), w = a.getWidth(), y0 = a.getCentreY(), h = a.getHeight() * 0.5f;
        g.setColour (on ? onTint() : ink());
        juce::Path p;
        if (kind == 0)      { p.startNewSubPath (x0, y0); p.lineTo (x0+w*0.25f, y0-h); p.lineTo (x0+w*0.75f, y0+h); p.lineTo (x0+w, y0); }
        else if (kind == 1) { p.startNewSubPath (x0, y0); for (int i = 1; i <= 20; ++i) { float t = i/20.0f; p.lineTo (x0+w*t, y0 - std::sin (t*6.283f)*h); } }
        else if (kind == 2) { p.startNewSubPath (x0, y0+h); p.lineTo (x0, y0-h); p.lineTo (x0+w*0.5f, y0-h); p.lineTo (x0+w*0.5f, y0+h); p.lineTo (x0+w, y0+h); p.lineTo (x0+w, y0-h); }
        else                { const float s[5] { 0.3f,-0.6f,0.5f,-0.2f,0.7f }; float px = x0; for (int i = 0; i < 5; ++i) { float ny = y0 - s[i]*h; p.startNewSubPath (px, ny); p.lineTo (px+w/5.0f, ny); px += w/5.0f; } }
        g.strokePath (p, juce::PathStrokeType (1.6f));
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

            // --- top bar (taller: preset, 8 MACRO knobs, big master, rec, indicators) ---
            auto top = takeT (area, 80);
            g.setColour (panelLt()); g.fillRoundedRectangle (top.toFloat(), 6.0f);
            auto tb = top.reduced (10, 7);
            auto preset = takeL (tb, 210);
            g.setColour (track()); g.fillRoundedRectangle (preset.removeFromTop (34).toFloat(), 5.0f);
            g.setColour (ink()); g.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
            g.drawText ("  synth   -   Fat Saw Bass", preset.translated (0, -23), juce::Justification::centredLeft, false);
            g.setColour (dim()); g.setFont (juce::Font (juce::FontOptions (11.0f)));
            g.drawText ("  CPU 12%   MIDI ok   CLK 120 int   SCOPE [on]", preset, juce::Justification::centredLeft, false);
            // right cluster: help, master (bigger), rec
            auto help = tb.removeFromRight (36); g.setColour (track()); g.fillRoundedRectangle (help.reduced (0, 18).toFloat(), 5.0f);
            g.setColour (ink()); g.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold))); g.drawText ("?", help.reduced (0, 18), juce::Justification::centred, false);
            tb.removeFromRight (6);
            knob (g, tb.removeFromRight (90), "MASTER", 0.7f);                 // bigger master
            auto rec = tb.removeFromRight (74).reduced (0, 24); g.setColour (Colour (0xffd8443a)); g.fillEllipse (rec.removeFromLeft (16).toFloat());
            g.setColour (ink()); g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold))); g.drawText (" REC", rec, juce::Justification::centredLeft, false);
            tb.removeFromRight (10);
            // 8 MACRO knobs fill the middle
            for (int m = 0; m < 8; ++m)
            { auto mc = takeL (tb, tb.getWidth() / (8 - m));
              knob (g, mc, "M" + String (m + 1), 0.3f + 0.08f * m); }

            // --- BOTTOM WORKSTATION: chord + RHYTHM + LOOPER -------------------
            auto work = area.removeFromBottom (juce::jmax (250, area.getHeight() * 38 / 100));
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
                { auto lane = rb; const int cells = 16; const int cw = lane.getWidth() / cells;
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
                const int lh = (lb.getHeight() - 3 * 5) / 4;
                for (int i = 0; i < 4; ++i)
                { auto lane = takeT (lb, lh);
                  g.setColour (track()); g.fillRoundedRectangle (lane.toFloat(), 4.0f);
                  g.setColour (ink()); g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
                  g.drawText (lanes[i], lane.removeFromLeft (70).withTrimmedLeft (8), juce::Justification::centredLeft, false);
                  if (i < 3) { g.setColour (tLoop.withAlpha (0.5f)); juce::Random rr (i + 3);
                    for (int x = 0; x < lane.getWidth(); x += 6) { float h = lane.getHeight() * (0.2f + 0.7f * rr.nextFloat());
                      g.fillRect (Rectangle<float> ((float) (lane.getX() + x), lane.getCentreY() - h / 2, 3.0f, h)); } } }
            }

            // --- left PART RAIL (even-height cells) -----------------------------
            auto rail = takeL (area, 176);
            {
                auto rl = section (g, rail, "Parts", tParts);
                const char* pn[]  { "P1  LIVE", "P2  (spare)", "P3  Fat Saw Bass", "P4  808 Basics" };
                const char* sub[] { "Bright Lead", "tap to add", "bass", "kit  -  6 pads" };
                const int cellH = (rl.getHeight() - 3 * 5) / 4;      // ONE height -> even cells
                for (int i = 0; i < 4; ++i)
                {
                    auto cell = takeT (rl, cellH);
                    const bool selp = i == 1;
                    g.setColour (selp ? track().brighter (0.16f) : track()); g.fillRoundedRectangle (cell.toFloat(), 6.0f);
                    if (selp) { g.setColour (accent()); g.drawRoundedRectangle (cell.toFloat().reduced (1), 6.0f, 2.0f); }
                    auto lvl = cell.removeFromRight (24).reduced (4, 7);
                    g.setColour (track().darker (0.35f)); g.fillRoundedRectangle (lvl.toFloat(), 3.0f);
                    g.setColour (accent()); g.fillRoundedRectangle (lvl.withTrimmedTop (lvl.getHeight() / 3).toFloat(), 3.0f);
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

            // --- centre synth --------------------------------------------------
            auto centre = area;
            const int u = juce::jmax (78, (centre.getWidth() - 4 * 5) / 13);

            // OSCILLATORS: one sub-box per osc; wave buttons across top, knobs below.
            { auto s = section (g, takeL (centre, u * 3), "Oscillators", tOsc);
              const int bh = (s.getHeight() - 2 * 5) / 3;
              for (int i = 0; i < 3; ++i)
              { auto box = subBox (g, takeT (s, bh), tOsc);
                selector (g, takeT (box, 28), { "SAW","SQR","TRI","SIN","WT.." }, i);
                for (auto* l : { "OCTAVE","DETUNE","PW","FINE" }) knob (g, takeL (box, box.getWidth()/(l[0]=='O'?4:l[0]=='D'?3:l[1]=='W'?2:1)), l); } }

            // FILTER: type buttons 50% taller; all knobs stacked vertically, equal size.
            { auto s = section (g, takeL (centre, u * 2), "Filter", tFilt);
              selector (g, takeT (s, 50), { "LP","HP","BP","NOTCH" }, 0);   // 50% taller
              const char* fk[] { "CUTOFF","RESO","DRIVE","ENV AMT","KEYTRK" };
              const float fv[] { 0.7f, 0.35f, 0.2f, 0.5f, 0.4f };
              const int kh = (s.getHeight() - 4 * 5) / 5;
              for (int i = 0; i < 5; ++i) { auto row = takeT (s, kh);
                knobAt (g, row.removeFromLeft (kh + 6), fv[i], 60);
                g.setColour (ink()); g.setFont (juce::Font (juce::FontOptions (11.5f, juce::Font::bold)));
                g.drawText (fk[i], row.withTrimmedLeft (4), juce::Justification::centredLeft, false); } }

            // ENVELOPE: unchanged.
            { auto s = section (g, takeL (centre, u * 2), "Envelope", tEnv);
              selector (g, takeT (s, 30), { "AMP","MOD" }, 0);
              auto kk = s.removeFromRight (s.getWidth() * 2 / 6);
              for (auto* l : { "A","D","S","R" }) fader (g, takeL (s, s.getWidth()/(l[0]=='A'?4:l[0]=='D'?3:l[0]=='S'?2:1)), l, 0.55f);
              knob (g, takeT (kk, kk.getHeight()/2), "E>PCH"); knob (g, kk, "VEL"); }

            // LFO: one sub-box per LFO; DEST selector horizontal across the top, knobs below.
            { auto s = section (g, takeL (centre, u * 3), "LFO", tLfo);
              const int bh = (s.getHeight() - 2 * 5) / 3;
              for (int i = 0; i < 3; ++i)
              { auto box = subBox (g, takeT (s, bh), tLfo);
                selector (g, takeT (box, 26), { "PITCH","CUTOFF","PW","OFF" }, i);
                auto shapes = box.removeFromRight (40);                    // shape icons stacked vertically
                knob (g, takeL (box, box.getWidth()/2), "RATE"); knob (g, box, "DEPTH");
                const int ih = (shapes.getHeight() - 3 * 3) / 4;
                for (int k = 0; k < 4; ++k) { auto cell = shapes.removeFromTop (ih); shapes.removeFromTop (3); shapeIcon (g, cell, k, k == 1); } } }

            // FX: real draggable panel - Chorus / Delay / Reverb / Width, knobs + values.
            { auto s = section (g, centre, "FX", tFx);
              struct K { const char* l; const char* v; float p; };
              struct B { const char* name; bool on; std::vector<K> k; };
              std::vector<B> blocks = {
                { "CHORUS", true,  { { "RATE","0.80",0.6f }, { "DEPTH","0.50",0.5f }, { "MIX","0.50",0.5f } } },
                { "DELAY",  true,  { { "TIME","300ms",0.5f }, { "FBK","0.35",0.35f }, { "PNG","1.0",0.7f }, { "MIX","0.35",0.35f } } },
                { "REVERB", true,  { { "SIZE","0.50",0.5f }, { "DAMP","0.50",0.5f }, { "WIDTH","1.0",0.6f }, { "MIX","0.30",0.3f } } },
                { "WIDTH",  false, { { "WIDTH","1.40",0.7f } } } };
              const int bh = (s.getHeight() - 3 * 5) / 4;
              for (auto& b : blocks)
              { auto box = takeT (s, bh);
                g.setColour (panel()); g.fillRoundedRectangle (box.toFloat(), 5.0f);
                // backlit NAME BAR across the top = the on/off (glows when on, dark when off).
                auto bar = box.removeFromTop (26);
                g.setColour (b.on ? tFx : track().darker (0.35f)); g.fillRoundedRectangle (bar.toFloat(), 4.0f);
                if (b.on) { g.setColour (tFx.brighter (0.5f)); g.drawRoundedRectangle (bar.toFloat().reduced (0.7f), 4.0f, 1.0f); }
                grip (g, bar.removeFromLeft (18));
                g.setColour (b.on ? onTint() : dim()); g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
                g.drawText (b.name, bar.withTrimmedLeft (2), juce::Justification::centredLeft, false);
                box.removeFromTop (3);
                const int kw = box.getWidth() / (int) b.k.size();
                for (auto& kn : b.k) knobV (g, takeL (box, kw - 5), kn.l, kn.v, kn.p); }
            }
        }
    };

    void render (int w, int h, const String& name)
    {
        LayoutMockup m; m.setSize (w, h);
        auto img = m.createComponentSnapshot (m.getLocalBounds(), false, 1.0f);
        REQUIRE (img.isValid());
        juce::File out (String (VASYNTH_DOCS_DIR) + "/" + name);
        out.deleteFile();
        juce::FileOutputStream os (out); REQUIRE (os.openedOk());
        juce::PNGImageFormat png; REQUIRE (png.writeImageToStream (img, os));
    }
}

TEST_CASE ("R2 layout mockup renders (fullsize)", "[plugin][mockup][screenshot]")
{
    juce::ScopedJuceInitialiser_GUI init;
    render (1760, 980, "mockup-default.png");
}
