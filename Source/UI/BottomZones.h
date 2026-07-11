#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "VASynthLookAndFeel.h"
#include "PanelChrome.h"
#include "Widgets.h"
#include "../PluginProcessor.h"
#include "../DSP/ChordEngine.h"

// ============================================================================
// Bottom workstation: a horizontal CHORD bar (enable / root / scale + the seven
// momentary, MIDI-learnable modifier chips) and two collapsible zones — RHYTHM
// (arp + sequencer) and LOOPER (per-part MIDI loops + export). The rhythm/looper
// ENGINES arrive in R3; here they are collapsed-by-default placeholders that
// expand to preview their planned home. Refuses keyboard focus.
// ============================================================================

// ---- horizontal chord bar --------------------------------------------------
class ChordBar : public juce::Component,
                 private juce::Timer
{
public:
    explicit ChordBar (VASynthProcessor& p) : proc (p)
    {
        setWantsKeyboardFocus (false);
        enable = std::make_unique<PowerToggle> (p.apvts, ParamID::chordEnabled, "CHORD");
        root   = std::make_unique<HSelector> (p.apvts, ParamID::chordRoot,  p.getMidiLearn());
        scale  = std::make_unique<HSelector> (p.apvts, ParamID::chordScale, p.getMidiLearn(),
                                              juce::StringArray { "MAJ", "MIN" });
        addAndMakeVisible (*enable); addAndMakeVisible (*root); addAndMakeVisible (*scale);
        startTimerHz (20);
    }

    void paint (juce::Graphics& g) override
    {
        auto c = chrome::section (g, getLocalBounds(), "Chord", juce::Colour (0xffe0733a));
        // modifier chips fill the region to the right of the selectors (laid out in resized).
        const int n = ChordEngine::kNumModifiers;
        const int cw = juce::jmax (1, chipArea.getWidth() / n);
        for (int i = 0; i < n; ++i)
        {
            auto cell = juce::Rectangle<int> (chipArea.getX() + i * cw, chipArea.getY(), cw, chipArea.getHeight()).reduced (2);
            chipBounds[(std::size_t) i] = cell;
            const bool lit = proc.isModifierActive (i);
            const bool learning = proc.getModifierLearn().isLearningModifier (i);
            g.setColour (lit ? VASynthLookAndFeel::accent() : VASynthLookAndFeel::track());
            g.fillRoundedRectangle (cell.toFloat(), 4.0f);
            if (learning)
            {
                const float a = 0.4f + 0.4f * (float) std::abs (std::sin (juce::Time::getMillisecondCounter() * 0.006));
                g.setColour (VASynthLookAndFeel::accentWarm().withAlpha (a));
                g.drawRoundedRectangle (cell.toFloat().reduced (1.0f), 4.0f, 2.0f);
            }
            g.setColour (lit ? juce::Colours::black : VASynthLookAndFeel::ink());
            g.setFont (juce::Font (juce::FontOptions (11.5f, juce::Font::bold)));
            g.drawText (ChordEngine::modifierName (i), cell, juce::Justification::centred, false);

            const int cc = proc.getModifierLearn().getCCForModifier (i);
            const int nn = proc.getModifierLearn().getNoteForModifier (i);
            if (cc >= 0 || nn >= 0)
            {
                auto badge = cell.removeFromBottom (11).toFloat().reduced (2.0f);
                g.setColour (VASynthLookAndFeel::accentWarm().withAlpha (0.9f));
                g.fillRoundedRectangle (badge, 2.0f);
                g.setColour (juce::Colours::black);
                g.setFont (juce::Font (juce::FontOptions (8.5f, juce::Font::bold)));
                g.drawText (cc >= 0 ? ("CC" + juce::String (cc)) : ("N" + juce::String (nn)), badge, juce::Justification::centred, false);
            }
        }
    }

    void resized() override
    {
        auto c = chrome::sectionContent (getLocalBounds());
        enable->setBounds (c.removeFromLeft (86).reduced (2, 4)); c.removeFromLeft (8);
        root->setBounds  (c.removeFromLeft (juce::jmin (330, c.getWidth() / 2)).reduced (0, 4)); c.removeFromLeft (8);
        scale->setBounds (c.removeFromLeft (120).reduced (0, 4)); c.removeFromLeft (10);
        chipArea = c;
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        for (int i = 0; i < ChordEngine::kNumModifiers; ++i)
            if (chipBounds[(std::size_t) i].contains (e.getPosition()))
            {
                if (e.mods.isPopupMenu()) { showLearnMenu (i); return; }
                pressCell = i; pressStart = juce::Time::getMillisecondCounter(); return;
            }
        pressCell = -1;
    }
    void mouseUp (const juce::MouseEvent&) override { pressCell = -1; }

private:
    void showLearnMenu (int modId)
    {
        auto& ml = proc.getModifierLearn();
        juce::PopupMenu m;
        m.addItem (1, "Learn " + juce::String (ChordEngine::modifierName (modId)) + " from a CC/pad");
        if (ml.getCCForModifier (modId) >= 0 || ml.getNoteForModifier (modId) >= 0) m.addItem (2, "Clear learned source");
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                         [&ml, modId] (int r) { if (r == 1) ml.armLearn (modId); else if (r == 2) ml.clearModifier (modId); });
    }
    void timerCallback() override
    {
        if (pressCell >= 0 && juce::Time::getMillisecondCounter() - pressStart > 500)
        { proc.getModifierLearn().armLearn (pressCell); pressCell = -1; }
        repaint();
    }

    VASynthProcessor& proc;
    std::unique_ptr<PowerToggle> enable;
    std::unique_ptr<HSelector> root, scale;
    juce::Rectangle<int> chipArea;
    std::array<juce::Rectangle<int>, ChordEngine::kNumModifiers> chipBounds {};
    int pressCell = -1;
    juce::uint32 pressStart = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChordBar)
};

// ---- RHYTHM: functional arpeggiator + editable 16-step sequencer ------------
class RhythmPanel : public juce::Component,
                    private juce::Timer
{
public:
    explicit RhythmPanel (VASynthProcessor& p) : proc (p)
    {
        setWantsKeyboardFocus (false);
        arpOn = std::make_unique<PowerToggle> (p.apvts, ParamID::arpOn, "ARP");
        mode  = std::make_unique<HSelector> (p.apvts, ParamID::arpMode, p.getMidiLearn(),
                                             juce::StringArray { "UP", "DOWN", "UP-DN", "RAND", "PLAYED" });
        oct   = std::make_unique<RotaryKnob> (p.apvts, ParamID::arpOctaves, "OCT",   p.getMidiLearn());
        gate  = std::make_unique<RotaryKnob> (p.apvts, ParamID::arpGate,    "GATE",  p.getMidiLearn());
        swing = std::make_unique<RotaryKnob> (p.apvts, ParamID::arpSwing,   "SWING", p.getMidiLearn());
        tempo = std::make_unique<RotaryKnob> (p.apvts, ParamID::tempo,      "TEMPO", p.getMidiLearn());
        latch = std::make_unique<PowerToggle> (p.apvts, ParamID::arpLatch, "LATCH");
        hold  = std::make_unique<PowerToggle> (p.apvts, ParamID::arpHold,  "HOLD");
        addAndMakeVisible (*arpOn); addAndMakeVisible (*mode);
        addAndMakeVisible (*oct);   addAndMakeVisible (*gate);
        addAndMakeVisible (*swing); addAndMakeVisible (*tempo);
        addAndMakeVisible (*latch); addAndMakeVisible (*hold);
        startTimerHz (20);   // playhead + reflect external step changes
    }

    void paint (juce::Graphics& g) override
    {
        const auto tRhy = juce::Colour (0xffe0b13a);
        chrome::section (g, getLocalBounds(), "Rhythm  -  arp + sequencer", tRhy);

        const int play = proc.arpDisplayStep();
        const int cells = VASynthProcessor::kArpSteps;
        const int cw = juce::jmax (1, gridArea.getWidth() / cells);
        for (int s = 0; s < cells; ++s)
        {
            auto cell = juce::Rectangle<int> (gridArea.getX() + s * cw, gridArea.getY(), cw, gridArea.getHeight()).reduced (2);
            g.setColour (VASynthLookAndFeel::track());
            g.fillRoundedRectangle (cell.toFloat(), 3.0f);
            const float v = proc.getArpStep (s);
            if (v > 0.001f)
            {
                auto bar = cell.removeFromBottom (juce::roundToInt (cell.getHeight() * v));
                g.setColour ((s == play) ? tRhy.brighter (0.3f) : tRhy);
                g.fillRoundedRectangle (bar.toFloat(), 3.0f);
            }
            if (s == play) { g.setColour (VASynthLookAndFeel::ink().withAlpha (0.85f)); g.drawRoundedRectangle (cell.toFloat(), 3.0f, 1.5f); }
        }
    }

    void resized() override
    {
        auto c = chrome::sectionContent (getLocalBounds());
        auto row = c.removeFromTop (juce::jmin (66, c.getHeight() / 2)); c.removeFromTop (6);
        arpOn->setBounds (row.removeFromLeft (58).reduced (2, 6)); row.removeFromLeft (6);
        mode->setBounds  (row.removeFromLeft (juce::jmin (230, row.getWidth() / 2)).reduced (0, 8)); row.removeFromLeft (8);
        for (RotaryKnob* k : { oct.get(), gate.get(), swing.get(), tempo.get() })
            k->setBounds (row.removeFromLeft (juce::jmin (58, row.getWidth() / 4)));
        row.removeFromLeft (6);
        latch->setBounds (row.removeFromLeft (60).reduced (2, 6));
        hold->setBounds  (row.removeFromLeft (60).reduced (2, 6));
        gridArea = c;
    }

    void mouseDown (const juce::MouseEvent& e) override { paintStep (e); }
    void mouseDrag (const juce::MouseEvent& e) override { paintStep (e); }

private:
    void paintStep (const juce::MouseEvent& e)
    {
        if (! gridArea.contains (e.getPosition())) return;
        const int cells = VASynthProcessor::kArpSteps;
        const int cw = juce::jmax (1, gridArea.getWidth() / cells);
        const int s = juce::jlimit (0, cells - 1, (e.getPosition().x - gridArea.getX()) / cw);
        float v = 1.0f - (float) (e.getPosition().y - gridArea.getY()) / juce::jmax (1, gridArea.getHeight());
        v = juce::jlimit (0.0f, 1.0f, v);
        if (v < 0.06f) v = 0.0f;                 // snap the bottom to a rest
        proc.setArpStep (s, v);
        repaint();
    }
    void timerCallback() override { repaint(); }

    VASynthProcessor& proc;
    std::unique_ptr<PowerToggle> arpOn, latch, hold;
    std::unique_ptr<HSelector> mode;
    std::unique_ptr<RotaryKnob> oct, gate, swing, tempo;
    juce::Rectangle<int> gridArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RhythmPanel)
};

// ---- LOOPER: functional per-part MIDI looper + MIDI export ------------------
class LooperPanel : public juce::Component,
                    private juce::Timer
{
public:
    explicit LooperPanel (VASynthProcessor& p) : proc (p)
    {
        setWantsKeyboardFocus (false);
        rec  = std::make_unique<PowerToggle> (p.apvts, ParamID::loopRec,  "REC");
        play = std::make_unique<PowerToggle> (p.apvts, ParamID::loopPlay, "PLAY");
        sync = std::make_unique<HSelector> (p.apvts, ParamID::loopBars, p.getMidiLearn(),
                                            juce::StringArray { "1 BAR", "2 BAR", "4 BAR" });
        addAndMakeVisible (*rec); addAndMakeVisible (*play); addAndMakeVisible (*sync);

        auto styleBtn = [] (juce::TextButton& b)
        {
            b.setWantsKeyboardFocus (false);
            b.setColour (juce::TextButton::buttonColourId, VASynthLookAndFeel::track());
            b.setColour (juce::TextButton::textColourOffId, VASynthLookAndFeel::ink());
        };
        clear.setButtonText ("CLEAR"); styleBtn (clear);
        clear.onClick = [this] { proc.clearLoops(); };
        addAndMakeVisible (clear);
        expo.setButtonText ("EXPORT MIDI"); styleBtn (expo);
        expo.onClick = [this] { exportMidi(); };
        addAndMakeVisible (expo);

        startTimerHz (20);   // playhead
    }

    void paint (juce::Graphics& g) override
    {
        const auto tLoop = juce::Colour (0xffca6bd0);
        chrome::section (g, getLocalBounds(), "Looper  -  per-part MIDI loops + session export", tLoop);

        const float ph = proc.loopPlayhead();
        for (int i = 0; i < 4; ++i)
        {
            auto lane = laneRects[(std::size_t) i];
            if (lane.isEmpty()) continue;
            g.setColour (VASynthLookAndFeel::track());
            g.fillRoundedRectangle (lane.toFloat(), 4.0f);
            g.setColour (VASynthLookAndFeel::ink());
            g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
            g.drawText ("P" + juce::String (i + 1), lane.removeFromLeft (48).withTrimmedLeft (8), juce::Justification::centredLeft, false);

            if (proc.loopLaneHasContent (i))
            {
                g.setColour (tLoop.withAlpha (0.35f));
                g.fillRoundedRectangle (lane.reduced (2, 4).toFloat(), 3.0f);
            }
            // playhead
            const int px = lane.getX() + juce::roundToInt (ph * lane.getWidth());
            g.setColour (tLoop.brighter (0.3f));
            g.fillRect (juce::Rectangle<int> (px - 1, lane.getY() + 2, 2, lane.getHeight() - 4));
        }
    }

    void resized() override
    {
        auto c = chrome::sectionContent (getLocalBounds());
        auto bar = c.removeFromTop (40); c.removeFromTop (6);
        rec->setBounds  (bar.removeFromLeft (70).reduced (2, 4));
        play->setBounds (bar.removeFromLeft (70).reduced (2, 4));
        clear.setBounds (bar.removeFromLeft (70).reduced (2, 4));
        sync->setBounds (bar.removeFromLeft (150).reduced (2, 4)); bar.removeFromLeft (8);
        expo.setBounds  (bar.removeFromRight (120).reduced (2, 4));

        const int lh = juce::jmax (1, (c.getHeight() - 3 * 5) / 4);
        for (int i = 0; i < 4; ++i) { laneRects[(std::size_t) i] = c.removeFromTop (lh); c.removeFromTop (5); }
    }

private:
    void exportMidi()
    {
        chooser = std::make_unique<juce::FileChooser> ("Export loops to MIDI",
                                                       juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("synth-loops.mid"),
                                                       "*.mid");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
            [this] (const juce::FileChooser& fc)
            {
                const auto f = fc.getResult();
                if (f != juce::File())
                    proc.postToast (proc.exportLoopsToMidiFile (f) ? "Loops exported to MIDI"
                                                                   : "Nothing recorded to export");
            });
    }
    void timerCallback() override { repaint(); }

    VASynthProcessor& proc;
    std::unique_ptr<PowerToggle> rec, play;
    std::unique_ptr<HSelector> sync;
    juce::TextButton clear, expo;
    std::array<juce::Rectangle<int>, 4> laneRects { };
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LooperPanel)
};

// ---- the whole bottom workstation ------------------------------------------
class BottomZones : public juce::Component
{
public:
    explicit BottomZones (VASynthProcessor& p) : chord (p), rhythm (p), looper (p)
    {
        setWantsKeyboardFocus (false);
        addAndMakeVisible (chord);
        addAndMakeVisible (rhythm);
        addAndMakeVisible (looper);
    }

    // Editor calls this to size the bottom band (fixed: chord bar + workstation).
    int preferredHeight() const { return kChordH + gap + kWorkH; }
    std::function<void()> onResizeNeeded;   // kept for API compatibility (unused now)

    void resized() override
    {
        auto r = getLocalBounds();
        chord.setBounds (r.removeFromTop (kChordH)); r.removeFromTop (gap);
        rhythm.setBounds (r.removeFromLeft (r.getWidth() * 55 / 100)); r.removeFromLeft (gap);
        looper.setBounds (r);
    }

private:
    static constexpr int kChordH = 64, kWorkH = 214, gap = 5;
    ChordBar chord;
    RhythmPanel rhythm;
    LooperPanel looper;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BottomZones)
};
