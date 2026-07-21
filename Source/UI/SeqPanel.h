#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "VASynthLookAndFeel.h"
#include "PanelChrome.h"
#include "Widgets.h"
#include "../PluginProcessor.h"

// ============================================================================
// STEP SEQUENCER panel (R3 Group 2): an 8-row x 16-step drum grid. Header carries the
// SEQ on/off, the target part (P1-P4) and gate. Each row shows its trigger note (tap to
// pick) + a mute. Step grammar (shared with the arp, #54): single tap a dark cell turns it
// on; double-tap a lit cell turns it off; touch-and-hold + vertical drag sets its velocity.
// A playhead column tracks the running step. The pattern lives in the processor state
// (saved with presets/MULTIs). Refuses keyboard focus.
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
            g.drawText (rowLabel (proc.getSeqNote (r)), lab.reduced (4, 0), juce::Justification::centredLeft, false);

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
                if (r == velR && s == velS)   // bright numeric read-out on the step being adjusted
                {
                    g.setColour (juce::Colours::black.withAlpha (0.6f)); g.fillRoundedRectangle (cell.toFloat(), 2.5f);
                    g.setColour (juce::Colours::white);
                    g.setFont (juce::Font (juce::FontOptions ((float) juce::jmin (14, cell.getHeight() - 1), juce::Font::bold)));
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

    // One grammar for the step cells (shared with the arp):
    //   - single TAP a DARK cell -> turn it ON
    //   - double-tap a LIT cell   -> turn it OFF   (a stray single tap never silences a step)
    //   - touch-and-HOLD a cell   -> velocity mode, then drag UP louder / DOWN quieter
    // The label column keeps its mute toggle + note-picker. Hold is detected by time
    // (kLongPressMs) OR a clear vertical drag; releasing a hold never toggles the cell.
    void mouseDown (const juce::MouseEvent& e) override
    {
        pressMode = Idle; dragR = dragS = -1; velR = velS = -1;
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
            pressMode = Pressed; pressDownMs = juce::Time::getMillisecondCounter();
            return;
        }
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragR < 0) return;
        const int dy = dragStartY - e.getPosition().y;   // up = louder
        if (pressMode == Pressed && dragWasOn && std::abs (dy) > 8) { pressMode = Velocity; velR = dragR; velS = dragS; }
        if (pressMode == Velocity)
        {
            proc.setSeqStepVel (dragR, dragS, juce::jlimit (10, 200, dragStartVel + (int) std::lround (dy * 1.4)));
            repaint();
        }
    }
    void mouseUp (const juce::MouseEvent&) override
    {
        if (pressMode == Pressed && dragR >= 0 && ! dragWasOn)         // a plain tap on a dark cell
            proc.setSeqCell (dragR, dragS, 1);                         // -> turn it ON (never off)
        pressMode = Idle; dragR = dragS = -1; velR = velS = -1; repaint();
    }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        for (int r = 0; r < VASynthProcessor::kSeqRows; ++r)
        {
            auto row = rowRects[(std::size_t) r];
            if (! row.contains (e.getPosition())) continue;
            if (! row.withTrimmedLeft (kLabelW).contains (e.getPosition())) break;   // label column: ignore
            const int s = colAt (row, e);
            if (proc.getSeqCell (r, s) != 0) proc.setSeqCell (r, s, 0);              // double-tap a lit cell -> OFF
            break;
        }
        pressMode = Idle; dragR = dragS = -1; velR = velS = -1; repaint();
    }

    // Centre of cell (row r, step s) in panel-local coords (for tests + external hit-testing).
    juce::Point<int> stepCellCentre (int r, int s) const
    {
        if (r < 0 || r >= VASynthProcessor::kSeqRows) return {};
        auto steps = rowRects[(std::size_t) r].withTrimmedLeft (kLabelW);
        const int cw = juce::jmax (1, steps.getWidth() / VASynthProcessor::kSeqSteps);
        return { steps.getX() + s * cw + cw / 2, rowRects[(std::size_t) r].getCentreY() };
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

    // Note name, e.g. 36 -> "C1" (middle C = C3 = MIDI 60).
    static juce::String noteName (int note)
    {
        static const char* n[] { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
        return juce::String (n[((note % 12) + 12) % 12]) + juce::String (note / 12 - 2);
    }

    // Row label: pad name + trigger note (e.g. "Snare D1", "Snare D#1") so no two rows read the
    // same even when the GM name repeats. Unmapped triggers fall back to the note name alone.
    static juce::String rowLabel (int note)
    {
        const juce::String gm = noteLabel (note);
        return (gm == juce::String (note)) ? noteName (note) : (gm + " " + noteName (note));
    }
    void timerCallback() override
    {
        // Motionless finger-hold on a lit cell: promote to velocity mode after kLongPressMs.
        if (pressMode == Pressed && dragWasOn && dragR >= 0
            && juce::Time::getMillisecondCounter() - pressDownMs > (juce::uint32) kLongPressMs)
            { pressMode = Velocity; velR = dragR; velS = dragS; }
        repaint();
    }

    VASynthProcessor& proc;
    std::unique_ptr<PowerToggle> on;
    std::unique_ptr<HSelector> target;
    std::unique_ptr<RotaryKnob> gate;
    std::array<juce::Rectangle<int>, VASynthProcessor::kSeqRows> rowRects { };
    static constexpr int kLongPressMs = 300;
    enum PressMode { Idle = 0, Pressed, Velocity };
    int pressMode = Idle;
    int dragR = -1, dragS = -1, dragStartY = 0, dragStartVel = 100;
    bool dragWasOn = false;
    juce::uint32 pressDownMs = 0;
    int velR = -1, velS = -1;              // step currently showing its numeric velocity
    static constexpr int kLabelW = 74;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SeqPanel)
};
