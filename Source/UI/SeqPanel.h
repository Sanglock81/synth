#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "VASynthLookAndFeel.h"
#include "PanelChrome.h"
#include "Widgets.h"
#include "../PluginProcessor.h"

// ============================================================================
// STEP SEQUENCER panel (R3 Group 2): an 8-row x 16-step drum grid. Header carries the
// SEQ on/off, the target part (P1-P4) and gate. Each row shows its trigger note (tap to
// pick) + a mute; each step is tap-cycled off -> on -> ACCENT -> off. A playhead column
// tracks the running step. The pattern lives in the processor state (saved with presets/
// MULTIs). Refuses keyboard focus.
// ============================================================================

class SeqPanel : public juce::Component,
                 private juce::Timer
{
public:
    explicit SeqPanel (VASynthProcessor& p) : proc (p)
    {
        setWantsKeyboardFocus (false);
        on     = std::make_unique<PowerToggle> (p.apvts, ParamID::seqOn, "SEQ");
        target = std::make_unique<HSelector> (p.apvts, ParamID::seqTarget, p.getMidiLearn(),
                                              juce::StringArray { "P1", "P2", "P3", "P4" });
        gate   = std::make_unique<RotaryKnob> (p.apvts, ParamID::seqGate, "GATE", p.getMidiLearn());
        addAndMakeVisible (*on); addAndMakeVisible (*target); addAndMakeVisible (*gate);
        startTimerHz (20);   // playhead
    }

    void paint (juce::Graphics& g) override
    {
        chrome::section (g, getLocalBounds(), "Sequencer  -  drum grid", juce::Colour (0xffe0b13a));
        const auto tRhy = juce::Colour (0xffe0b13a);
        const int play = proc.seqDisplayStep();

        for (int r = 0; r < VASynthProcessor::kSeqRows; ++r)
        {
            auto row = rowRects[(std::size_t) r];
            if (row.isEmpty()) continue;
            auto lab = row.removeFromLeft (kLabelW);
            // trigger-note label + mute
            const bool muted = proc.getSeqMute (r);
            g.setColour (muted ? VASynthLookAndFeel::track() : VASynthLookAndFeel::track().brighter (0.12f));
            g.fillRoundedRectangle (lab.reduced (1).toFloat(), 3.0f);
            auto muteR = lab.removeFromRight (18);
            g.setColour (muted ? juce::Colour (0xffd8443a) : VASynthLookAndFeel::dim());
            g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
            g.drawText ("M", muteR, juce::Justification::centred, false);
            g.setColour (muted ? VASynthLookAndFeel::dim() : VASynthLookAndFeel::ink());
            g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
            g.drawText (noteLabel (proc.getSeqNote (r)), lab.reduced (4, 0), juce::Justification::centredLeft, false);

            // 16 step cells
            const int cw = juce::jmax (1, row.getWidth() / VASynthProcessor::kSeqSteps);
            for (int s = 0; s < VASynthProcessor::kSeqSteps; ++s)
            {
                auto cell = juce::Rectangle<int> (row.getX() + s * cw, row.getY(), cw, row.getHeight()).reduced (1);
                const bool cellOn = proc.getSeqCell (r, s) != 0;
                const int  vel = proc.getSeqStepVel (r, s);   // 10..200
                juce::Colour base = (s % 4 == 0) ? VASynthLookAndFeel::track().brighter (0.06f) : VASynthLookAndFeel::track();   // beat guides
                g.setColour (base);
                g.fillRoundedRectangle (cell.toFloat(), 2.5f);
                if (cellOn)
                {
                    // Velocity as a bottom-up FILL (height ∝ velocity, accent = >100% brightens).
                    const float frac = juce::jlimit (0.12f, 1.0f, vel / 150.0f);
                    auto fill = cell.toFloat().removeFromBottom (cell.getHeight() * frac);
                    juce::Colour c = vel > 100 ? tRhy.brighter ((vel - 100) / 120.0f) : tRhy;
                    g.setColour (muted ? c.withAlpha (0.4f) : c);
                    g.fillRoundedRectangle (fill, 2.5f);
                }
                if (s == play) { g.setColour (VASynthLookAndFeel::ink().withAlpha (0.7f)); g.drawRoundedRectangle (cell.toFloat(), 2.5f, 1.2f); }
                if (r == velR && s == velS)   // numeric readout while dragging velocity
                {
                    g.setColour (VASynthLookAndFeel::ink());
                    g.setFont (juce::Font (juce::FontOptions ((float) juce::jmin (12, cell.getHeight() - 2), juce::Font::bold)));
                    g.drawText (juce::String (vel), cell, juce::Justification::centred, false);
                }
            }
        }
    }

    void resized() override
    {
        auto c = chrome::sectionContent (getLocalBounds());
        auto head = c.removeFromTop (26); c.removeFromTop (4);
        on->setBounds (head.removeFromLeft (58).reduced (2, 2)); head.removeFromLeft (6);
        head.removeFromLeft (36);   // "TARGET" gap
        target->setBounds (head.removeFromLeft (150).reduced (0, 2)); head.removeFromLeft (8);
        gate->setBounds (head.removeFromRight (54));

        const int n = VASynthProcessor::kSeqRows, gap = 2;
        const int rh = juce::jmax (10, (c.getHeight() - (n - 1) * gap) / n);
        for (int r = 0; r < n; ++r) { rowRects[(std::size_t) r] = c.removeFromTop (rh); c.removeFromTop (gap); }
    }

    // Gesture: TAP a step toggles it on/off; HOLD an on-step and drag up/down sets its
    // velocity % (numeric readout while dragging); a horizontal drag paints on/off across
    // cells (draw a pattern). Which gesture wins is decided on the first drag delta (#54).
    void mouseDown (const juce::MouseEvent& e) override
    {
        dragR = dragS = -1; dragMode = None;
        for (int r = 0; r < VASynthProcessor::kSeqRows; ++r)
        {
            auto row = rowRects[(std::size_t) r];
            if (! row.contains (e.getPosition())) continue;
            auto lab = row.withWidth (kLabelW);
            if (lab.withTrimmedLeft (kLabelW - 18).contains (e.getPosition())) { proc.setSeqMute (r, ! proc.getSeqMute (r)); repaint(); return; }
            if (lab.contains (e.getPosition())) { showNoteMenu (r); return; }
            dragR = r; dragS = colAt (row, e); dragStartY = e.getPosition().y;
            dragStartVel = proc.getSeqStepVel (r, dragS);
            dragWasOn = proc.getSeqCell (r, dragS) != 0;
            return;
        }
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragR < 0) return;
        const int dy = dragStartY - e.getPosition().y;   // up = increase
        if (dragMode == None)
        {
            if (dragWasOn && std::abs (dy) > 5) { dragMode = Velocity; velR = dragR; velS = dragS; }
            else                                                                          // fall to paint
            {
                dragMode = Paint;
                dragVal = (unsigned char) (dragWasOn ? 0 : 1);                             // tap-through starts the stroke
                proc.setSeqCell (dragR, dragS, dragVal);
                repaint();
            }
        }
        if (dragMode == Velocity)
        {
            const int nv = juce::jlimit (10, 200, dragStartVel + (int) std::lround (dy * 1.4));
            proc.setSeqStepVel (dragR, dragS, nv);
            repaint();
        }
        else if (dragMode == Paint)
        {
            for (int r = 0; r < VASynthProcessor::kSeqRows; ++r)
            {
                auto row = rowRects[(std::size_t) r];
                if (! row.contains (e.getPosition())) continue;
                if (! row.withTrimmedLeft (kLabelW).contains (e.getPosition())) return;
                const int s = colAt (row, e);
                if (proc.getSeqCell (r, s) != dragVal) { proc.setSeqCell (r, s, dragVal); repaint(); }
                return;
            }
        }
    }
    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragMode == None && dragR >= 0)                                                // a plain tap toggles on/off
            proc.setSeqCell (dragR, dragS, (unsigned char) (dragWasOn ? 0 : 1));
        dragMode = None; dragR = dragS = -1; velR = velS = -1;
        repaint();
    }

private:
    // step column under the cursor for a row rect whose label has already been trimmed off
    static int colAt (juce::Rectangle<int> row, const juce::MouseEvent& e)
    {
        auto steps = row.withTrimmedLeft (kLabelW);
        const int cw = juce::jmax (1, steps.getWidth() / VASynthProcessor::kSeqSteps);
        return juce::jlimit (0, VASynthProcessor::kSeqSteps - 1, (e.getPosition().x - steps.getX()) / cw);
    }

    void showNoteMenu (int row)
    {
        juce::PopupMenu m;
        for (int n = 35; n <= 51; ++n) m.addItem (n, noteLabel (n) + "  (" + juce::String (n) + ")", true, n == proc.getSeqNote (row));
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                         [this, row] (int r) { if (r > 0) { proc.setSeqNote (row, r); repaint(); } });
    }

    static juce::String noteLabel (int note)
    {
        switch (note)   // common GM drum map for the default 808 range
        {
            case 35: case 36: return "Kick";
            case 37: return "Rim";  case 38: case 40: return "Snare";
            case 39: return "Clap"; case 41: case 43: case 45: return "Tom";
            case 42: return "Hat";  case 44: return "Pedal";  case 46: return "OpHat";
            case 49: return "Crash"; case 51: return "Ride";
            default: return juce::String (note);
        }
    }
    void timerCallback() override { repaint(); }

    VASynthProcessor& proc;
    std::unique_ptr<PowerToggle> on;
    std::unique_ptr<HSelector> target;
    std::unique_ptr<RotaryKnob> gate;
    std::array<juce::Rectangle<int>, VASynthProcessor::kSeqRows> rowRects { };
    enum DragMode { None = 0, Velocity, Paint };
    int dragMode = None;
    int dragR = -1, dragS = -1, dragStartY = 0, dragStartVel = 100;
    bool dragWasOn = false;
    unsigned char dragVal = 0;
    int velR = -1, velS = -1;              // step currently showing its numeric velocity
    static constexpr int kLabelW = 74;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SeqPanel)
};
