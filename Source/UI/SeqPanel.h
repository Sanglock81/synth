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
                const unsigned char v = proc.getSeqCell (r, s);
                juce::Colour c = (v == 2) ? tRhy.brighter (0.35f) : (v == 1) ? tRhy : VASynthLookAndFeel::track();
                if (s % 4 == 0 && v == 0) c = VASynthLookAndFeel::track().brighter (0.06f);   // beat guides
                g.setColour (muted && v > 0 ? c.withAlpha (0.4f) : c);
                g.fillRoundedRectangle (cell.toFloat(), 2.5f);
                if (s == play) { g.setColour (VASynthLookAndFeel::ink().withAlpha (0.7f)); g.drawRoundedRectangle (cell.toFloat(), 2.5f, 1.2f); }
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

    void mouseDown (const juce::MouseEvent& e) override { hit (e); }
    void mouseDrag (const juce::MouseEvent& e) override { if (dragPaint) paintDrag (e); }

private:
    void hit (const juce::MouseEvent& e)
    {
        for (int r = 0; r < VASynthProcessor::kSeqRows; ++r)
        {
            auto row = rowRects[(std::size_t) r];
            if (! row.contains (e.getPosition())) continue;
            auto lab = row.removeFromLeft (kLabelW);
            if (lab.removeFromRight (18).contains (e.getPosition())) { proc.setSeqMute (r, ! proc.getSeqMute (r)); repaint(); return; }
            if (lab.contains (e.getPosition())) { showNoteMenu (r); return; }
            const int cw = juce::jmax (1, row.getWidth() / VASynthProcessor::kSeqSteps);
            const int s = juce::jlimit (0, VASynthProcessor::kSeqSteps - 1, (e.getPosition().x - row.getX()) / cw);
            const unsigned char v = proc.getSeqCell (r, s);
            proc.setSeqCell (r, s, (unsigned char) ((v + 1) % 3));   // off -> on -> accent -> off
            dragPaint = true; dragVal = (unsigned char) ((v + 1) % 3);
            repaint(); return;
        }
    }
    void paintDrag (const juce::MouseEvent& e)
    {
        for (int r = 0; r < VASynthProcessor::kSeqRows; ++r)
        {
            auto row = rowRects[(std::size_t) r];
            if (! row.contains (e.getPosition())) continue;
            row.removeFromLeft (kLabelW);
            if (! row.contains (e.getPosition())) return;
            const int cw = juce::jmax (1, row.getWidth() / VASynthProcessor::kSeqSteps);
            const int s = juce::jlimit (0, VASynthProcessor::kSeqSteps - 1, (e.getPosition().x - row.getX()) / cw);
            if (proc.getSeqCell (r, s) != dragVal) { proc.setSeqCell (r, s, dragVal); repaint(); }
            return;
        }
    }
    void mouseUp (const juce::MouseEvent&) override { dragPaint = false; }

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
    bool dragPaint = false;
    unsigned char dragVal = 0;
    static constexpr int kLabelW = 74;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SeqPanel)
};
