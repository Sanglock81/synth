#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "VASynthLookAndFeel.h"
#include "PanelChrome.h"
#include "Widgets.h"
#include "SeqPanel.h"
#include "../PluginProcessor.h"
#include "../DSP/ChordEngine.h"

// ============================================================================
// Bottom workstation (R3 Group 2 layout): a horizontal CHORD bar (enable / root /
// scale + the seven momentary, MIDI-learnable modifier chips), a compact ARP bar
// (toggle / mode / OCT-GATE-SWING-TEMPO / HOLD, sharing the one transport clock),
// and, below, the 8-row step SEQUENCER (left) beside the per-part LOOPER (right).
// The arp and sequencer are decoupled: the arp reorders held notes; the sequencer
// runs its own drum grid into a selectable part. Refuses keyboard focus.
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
            g.setFont (juce::Font (juce::FontOptions (14.5f, juce::Font::bold)));
            g.drawText (ChordEngine::modifierName (i), cell, juce::Justification::centred, false);

            // QWERTY shortcut hint (standalone): the physical key that triggers this chip,
            // in the same left-to-right order as the keys c v b n m , . (top-left keycap).
            static const char* kModKeys[ChordEngine::kNumModifiers] { "C", "V", "B", "N", "M", ",", "." };
            g.setColour (lit ? juce::Colours::black.withAlpha (0.55f) : VASynthLookAndFeel::dim());
            g.setFont (juce::Font (juce::FontOptions (9.5f, juce::Font::bold)));
            g.drawText (kModKeys[i], cell.reduced (5, 3).removeFromTop (11), juce::Justification::topLeft, false);

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

// ---- ARP: compact arpeggiator bar (decoupled from the step sequencer) -------
// Controls on the left; a 16-square single-level ON/OFF gate grid fills the middle-
// right, giving the arp its own rhythm (a rest step is skipped) — independent of the
// SEQ drum grid. HOLD is the single latch source (LATCH was merged into HOLD in
// Group 2). OCT/GATE/SWING/TEMPO drive the shared transport clock the SEQ also rides.
class ArpBar : public juce::Component,
               private juce::Timer
{
public:
    explicit ArpBar (VASynthProcessor& p) : proc (p)
    {
        setWantsKeyboardFocus (false);
        arpOn = std::make_unique<PowerToggle> (p.apvts, ParamID::arpOn, "ARP");
        mode  = std::make_unique<HSelector> (p.apvts, ParamID::arpMode, p.getMidiLearn(),
                                             juce::StringArray { "UP", "DOWN", "UP-DN", "RAND", "PLAYED" });
        oct   = std::make_unique<RotaryKnob> (p.apvts, ParamID::arpOctaves, "OCT",   p.getMidiLearn());
        gate  = std::make_unique<RotaryKnob> (p.apvts, ParamID::arpGate,    "GATE",  p.getMidiLearn());
        swing = std::make_unique<RotaryKnob> (p.apvts, ParamID::arpSwing,   "SWING", p.getMidiLearn());
        tempo = std::make_unique<RotaryKnob> (p.apvts, ParamID::tempo,      "TEMPO", p.getMidiLearn());
        hold  = std::make_unique<PowerToggle> (p.apvts, ParamID::arpHold,  "HOLD");
        addAndMakeVisible (*arpOn); addAndMakeVisible (*mode);
        addAndMakeVisible (*oct);   addAndMakeVisible (*gate);
        addAndMakeVisible (*swing); addAndMakeVisible (*tempo);
        addAndMakeVisible (*hold);
        startTimerHz (20);   // playhead on the gate grid
    }

    void paint (juce::Graphics& g) override
    {
        const auto tRhy = juce::Colour (0xffe0b13a);
        chrome::section (g, getLocalBounds(), "Arpeggiator  -  gate pattern", tRhy);

        // 16-square single-level ON/OFF gate grid (middle-right).
        const int play = proc.arpDisplayStep();
        const int n = VASynthProcessor::kArpSteps;
        const int cw = juce::jmax (1, gridArea.getWidth() / n);
        for (int s = 0; s < n; ++s)
        {
            auto cell = juce::Rectangle<int> (gridArea.getX() + s * cw, gridArea.getY(), cw, gridArea.getHeight()).reduced (2);
            const bool on = proc.getArpStep (s) > 0.5f;
            juce::Colour c = on ? tRhy : VASynthLookAndFeel::track();
            if (! on && s % 4 == 0) c = VASynthLookAndFeel::track().brighter (0.07f);   // beat guides
            g.setColour (c);
            g.fillRoundedRectangle (cell.toFloat(), 3.0f);
            if (s == play && proc.apvts.getRawParameterValue (ParamID::arpOn)->load() > 0.5f)
            { g.setColour (VASynthLookAndFeel::ink().withAlpha (0.8f)); g.drawRoundedRectangle (cell.toFloat(), 3.0f, 1.5f); }
        }
    }

    void resized() override
    {
        auto c = chrome::sectionContent (getLocalBounds());
        auto row = c;
        arpOn->setBounds (row.removeFromLeft (58).reduced (2, 6)); row.removeFromLeft (8);
        mode->setBounds  (row.removeFromLeft (juce::jmin (210, row.getWidth() / 3)).reduced (0, 8)); row.removeFromLeft (12);
        hold->setBounds  (row.removeFromRight (62).reduced (2, 6)); row.removeFromRight (10);
        for (RotaryKnob* k : { oct.get(), gate.get(), swing.get(), tempo.get() })
            { k->setBounds (row.removeFromLeft (juce::jmin (72, row.getWidth() / 5))); row.removeFromLeft (4); }
        row.removeFromLeft (10);
        gridArea = row;                                   // the remaining middle-right span
    }

    void mouseDown (const juce::MouseEvent& e) override { paintStep (e, true); }
    void mouseDrag (const juce::MouseEvent& e) override { paintStep (e, false); }

private:
    // Tap toggles a step; drag paints the value set by the initial tap.
    void paintStep (const juce::MouseEvent& e, bool isDown)
    {
        if (! gridArea.contains (e.getPosition())) return;
        const int n = VASynthProcessor::kArpSteps;
        const int cw = juce::jmax (1, gridArea.getWidth() / n);
        const int s = juce::jlimit (0, n - 1, (e.getPosition().x - gridArea.getX()) / cw);
        if (isDown) dragVal = (proc.getArpStep (s) > 0.5f) ? 0.0f : 1.0f;   // toggle vs the tapped cell
        if ((proc.getArpStep (s) > 0.5f) != (dragVal > 0.5f)) { proc.setArpStep (s, dragVal); repaint(); }
    }
    void timerCallback() override { repaint(); }

    VASynthProcessor& proc;
    std::unique_ptr<PowerToggle> arpOn, hold;
    std::unique_ptr<HSelector> mode;
    std::unique_ptr<RotaryKnob> oct, gate, swing, tempo;
    juce::Rectangle<int> gridArea;
    float dragVal = 1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ArpBar)
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
        mode = std::make_unique<HSelector> (p.apvts, ParamID::loopMode, p.getMidiLearn(),
                                            juce::StringArray { "MIDI", "AUDIO" });
        sync = std::make_unique<HSelector> (p.apvts, ParamID::loopBars, p.getMidiLearn(),
                                            juce::StringArray { "1 BAR", "2 BAR", "4 BAR" });
        addAndMakeVisible (*rec); addAndMakeVisible (*play); addAndMakeVisible (*mode); addAndMakeVisible (*sync);

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
        expoWav.setButtonText ("EXPORT WAV"); styleBtn (expoWav);
        expoWav.onClick = [this] { exportWav(); };
        addAndMakeVisible (expoWav);

        startTimerHz (20);   // playhead + armed-record blink
    }

    void paint (juce::Graphics& g) override
    {
        const auto tLoop = juce::Colour (0xffca6bd0);
        chrome::section (g, getLocalBounds(), "Looper  -  MIDI + audio loops, session export", tLoop);

        // Armed / recording status pip (top-right of the header content).
        const int rs = proc.loopRecDisplayState();      // 0 idle, 1 armed, 2 recording
        if (! statusArea.isEmpty() && rs > 0)
        {
            const bool recOn = rs == 2;
            const float blink = 0.5f + 0.5f * (float) std::sin (juce::Time::getMillisecondCounter() * 0.008);
            const auto red = juce::Colour (0xffd8443a);
            g.setColour (recOn ? red : red.withAlpha (0.35f + 0.5f * blink));
            auto dot = statusArea.removeFromLeft (12).withSizeKeepingCentre (9, 9).toFloat();
            g.fillEllipse (dot);
            g.setColour (recOn ? red : VASynthLookAndFeel::dim());
            g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
            g.drawText (recOn ? "REC" : "ARMED", statusArea, juce::Justification::centredLeft, false);
        }

        const float ph = proc.loopPlayhead();
        const bool audioMode = proc.apvts.getRawParameterValue (ParamID::loopMode)->load() > 0.5f;
        for (int i = 0; i < 5; ++i)                       // 4 MIDI part lanes + 1 AUDIO lane
        {
            auto lane = laneRects[(std::size_t) i];
            if (lane.isEmpty()) continue;
            const bool isAudio = (i == 4);
            const bool hasContent = isAudio ? proc.loopAudioHasContent() : proc.loopLaneHasContent (i);
            const bool laneActive = isAudio ? audioMode : ! audioMode;   // which lane the mode plays

            g.setColour (VASynthLookAndFeel::track());
            g.fillRoundedRectangle (lane.toFloat(), 4.0f);
            g.setColour (laneActive ? VASynthLookAndFeel::ink() : VASynthLookAndFeel::dim());
            g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
            g.drawText (isAudio ? "AUD" : ("P" + juce::String (i + 1)),
                        lane.removeFromLeft (44).withTrimmedLeft (8), juce::Justification::centredLeft, false);

            if (hasContent)
            {
                const auto fill = isAudio ? juce::Colour (0xff58c0a8) : tLoop;   // audio lane a distinct hue
                g.setColour (fill.withAlpha (laneActive ? 0.4f : 0.18f));
                g.fillRoundedRectangle (lane.reduced (2, 3).toFloat(), 3.0f);
            }
            const int px = lane.getX() + juce::roundToInt (ph * lane.getWidth());
            g.setColour (tLoop.brighter (0.3f));
            g.fillRect (juce::Rectangle<int> (px - 1, lane.getY() + 2, 2, lane.getHeight() - 4));
        }
    }

    void resized() override
    {
        auto c = chrome::sectionContent (getLocalBounds());
        // Two control rows so REC/PLAY/CLEAR/MODE and BARS + the two exports all fit.
        auto rowA = c.removeFromTop (30); c.removeFromTop (5);
        rec->setBounds  (rowA.removeFromLeft (58).reduced (2, 2));
        play->setBounds (rowA.removeFromLeft (58).reduced (2, 2));
        clear.setBounds (rowA.removeFromLeft (58).reduced (2, 2)); rowA.removeFromLeft (8);
        mode->setBounds (rowA.removeFromLeft (juce::jmin (150, rowA.getWidth())).reduced (2, 2));

        auto rowB = c.removeFromTop (28); c.removeFromTop (6);
        sync->setBounds (rowB.removeFromLeft (170).reduced (2, 2)); rowB.removeFromLeft (8);
        expoWav.setBounds (rowB.removeFromRight (110).reduced (2, 2)); rowB.removeFromRight (4);
        expo.setBounds    (rowB.removeFromRight (110).reduced (2, 2));

        // Status pip lives in the header bar (drawn in paint), right-aligned.
        statusArea = juce::Rectangle<int> (getWidth() - 84, 4, 74, chrome::kHeaderH - 6);

        const int n = 5;
        const int lh = juce::jmax (1, (c.getHeight() - (n - 1) * 4) / n);
        for (int i = 0; i < n; ++i) { laneRects[(std::size_t) i] = c.removeFromTop (lh); c.removeFromTop (4); }
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
    void exportWav()
    {
        chooser = std::make_unique<juce::FileChooser> ("Export audio loop to WAV",
                                                       juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("synth-loop.wav"),
                                                       "*.wav");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
            [this] (const juce::FileChooser& fc)
            {
                const auto f = fc.getResult();
                if (f != juce::File())
                    proc.postToast (proc.exportLoopToWavFile (f) ? "Audio loop exported to WAV"
                                                                 : "No audio loop recorded to export");
            });
    }
    void timerCallback() override { repaint(); }

    VASynthProcessor& proc;
    std::unique_ptr<PowerToggle> rec, play;
    std::unique_ptr<HSelector> mode, sync;
    juce::TextButton clear, expo, expoWav;
    juce::Rectangle<int> statusArea;
    std::array<juce::Rectangle<int>, 5> laneRects { };   // 4 MIDI + 1 AUDIO
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LooperPanel)
};

// ---- the whole bottom workstation ------------------------------------------
class BottomZones : public juce::Component
{
public:
    explicit BottomZones (VASynthProcessor& p) : chord (p), arp (p), seq (p), looper (p)
    {
        setWantsKeyboardFocus (false);
        addAndMakeVisible (chord);
        addAndMakeVisible (arp);
        addAndMakeVisible (seq);
        addAndMakeVisible (looper);
    }

    // Editor calls this to size the bottom band: chord bar + arp bar + [seq | looper].
    int preferredHeight() const { return kChordH + gap + kArpH + gap + kGridH; }
    std::function<void()> onResizeNeeded;   // kept for API compatibility (unused now)

    void resized() override
    {
        auto r = getLocalBounds();
        chord.setBounds (r.removeFromTop (kChordH)); r.removeFromTop (gap);
        arp.setBounds   (r.removeFromTop (kArpH));   r.removeFromTop (gap);
        // 8-row sequencer needs the width; the looper takes the remainder on the right.
        seq.setBounds   (r.removeFromLeft (r.getWidth() * 58 / 100)); r.removeFromLeft (gap);
        looper.setBounds (r);
    }

private:
    // Taller than R2: chord chips ~2x tall (readability), arp holds the gate grid,
    // seq rows ~60% taller. The centre synth section flows shorter to make the room.
    static constexpr int kChordH = 92, kArpH = 74, kGridH = 268, gap = 5;
    ChordBar chord;
    ArpBar arp;
    SeqPanel seq;
    LooperPanel looper;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BottomZones)
};
