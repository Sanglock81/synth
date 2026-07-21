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

            // QWERTY shortcut hint (standalone): the physical key that triggers this chip, drawn
            // as a small bordered KEYCAP badge (top-left) so the keyboard mapping teaches itself
            // at arm's length. Same left-to-right order as the keys c v b n m , .
            static const char* kModKeys[ChordEngine::kNumModifiers] { "C", "V", "B", "N", "M", ",", "." };
            auto keycap = cell.reduced (4, 3).removeFromTop (13).removeFromLeft (13).toFloat();
            g.setColour ((lit ? juce::Colours::black : juce::Colours::white).withAlpha (0.10f));
            g.fillRoundedRectangle (keycap, 3.0f);
            g.setColour (lit ? juce::Colours::black.withAlpha (0.5f) : VASynthLookAndFeel::dim());
            g.drawRoundedRectangle (keycap, 3.0f, 1.0f);
            g.setColour (lit ? juce::Colours::black : VASynthLookAndFeel::ink());
            g.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::bold)));
            g.drawText (kModKeys[i], keycap, juce::Justification::centred, false);

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
        chrome::section (g, getLocalBounds(), "Arpeggiator", tRhy);

        // 16-step grid (middle-right): on/off + per-step velocity. A lit box plays that 16th;
        // its velocity shows as a bottom-up fill (height ∝ velocity; > 100 % brightens) AND as a
        // number in the box. Tap dark = on; double-tap lit = off; hold + drag = set velocity.
        const int play = proc.arpDisplayStep();
        const int n = VASynthProcessor::kArpSteps;
        const int cw = juce::jmax (1, gridArea.getWidth() / n);
        for (int s = 0; s < n; ++s)
        {
            auto cell = juce::Rectangle<int> (gridArea.getX() + s * cw, gridArea.getY(), cw, gridArea.getHeight()).reduced (2);
            const bool on  = proc.getArpStep (s) > 0.5f;
            const int  vel = proc.getArpStepVel (s);
            juce::Colour base = (s % 4 == 0) ? VASynthLookAndFeel::track().brighter (0.07f) : VASynthLookAndFeel::track();
            g.setColour (base);
            g.fillRoundedRectangle (cell.toFloat(), 3.0f);
            if (on)
            {
                const float frac = juce::jlimit (0.12f, 1.0f, vel / 150.0f);
                auto fill = cell.toFloat().removeFromBottom (cell.getHeight() * frac);
                g.setColour (vel > 100 ? tRhy.brighter ((vel - 100) / 120.0f) : tRhy);
                g.fillRoundedRectangle (fill, 3.0f);
            }
            if (s == play && proc.apvts.getRawParameterValue (ParamID::arpOn)->load() > 0.5f)
            { g.setColour (VASynthLookAndFeel::ink().withAlpha (0.8f)); g.drawRoundedRectangle (cell.toFloat(), 3.0f, 1.5f); }
            const bool adjusting = (s == velStep);
            if (on || adjusting)   // velocity number: bright while adjusting, dim at rest
            {
                if (adjusting) { g.setColour (juce::Colours::black.withAlpha (0.55f)); g.fillRoundedRectangle (cell.toFloat(), 3.0f); }
                g.setColour (adjusting ? juce::Colours::white : VASynthLookAndFeel::ink().withAlpha (0.85f));
                g.setFont (juce::Font (juce::FontOptions ((float) juce::jmin (adjusting ? 15 : 11, cell.getHeight() - 2), juce::Font::bold)));
                g.drawText (juce::String (vel), cell, juce::Justification::centred, false);
            }
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

    // One grammar for the step boxes (shared with the sequencer):
    //   - single TAP a DARK box  -> turn it ON
    //   - double-tap a LIT box    -> turn it OFF   (a stray single tap never silences a step)
    //   - touch-and-HOLD a box    -> enter velocity mode, then drag UP louder / DOWN quieter
    // The hold is detected by time (kLongPressMs) OR by a clear vertical drag, so a motionless
    // finger-hold still engages — releasing a hold does NOT toggle the step.
    void mouseDown (const juce::MouseEvent& e) override
    {
        pressMode = Idle; pressStep = -1; velStep = -1;
        if (! gridArea.contains (e.getPosition())) return;
        pressStep = colAt (e);
        pressStartY = e.getPosition().y;
        pressStartVel = proc.getArpStepVel (pressStep);
        pressWasOn = proc.getArpStep (pressStep) > 0.5f;
        pressMode = Pressed;
        pressDownMs = juce::Time::getMillisecondCounter();
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (pressStep < 0) return;
        const int dy = pressStartY - e.getPosition().y;                 // up = louder
        if (pressMode == Pressed && pressWasOn && std::abs (dy) > 8) { pressMode = Velocity; velStep = pressStep; }
        if (pressMode == Velocity)
        {
            proc.setArpStepVel (pressStep, juce::jlimit (10, 200, pressStartVel + (int) std::lround (dy * 1.4)));
            repaint();
        }
    }
    void mouseUp (const juce::MouseEvent&) override
    {
        if (pressMode == Pressed && pressStep >= 0 && ! pressWasOn)     // a plain tap on a dark box
            proc.setArpStep (pressStep, 1.0f);                          // -> turn it ON (never off)
        pressMode = Idle; pressStep = -1; velStep = -1; repaint();
    }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (gridArea.contains (e.getPosition()))
        {
            const int s = colAt (e);
            if (proc.getArpStep (s) > 0.5f) proc.setArpStep (s, 0.0f);  // double-tap a lit box -> OFF
        }
        pressMode = Idle; pressStep = -1; velStep = -1; repaint();
    }

    // Read-only geometry of the 16-step grid (for tests + external hit-testing).
    juce::Rectangle<int> stepGridBounds() const { return gridArea; }

private:
    int colAt (const juce::MouseEvent& e) const
    {
        const int cw = juce::jmax (1, gridArea.getWidth() / VASynthProcessor::kArpSteps);
        return juce::jlimit (0, VASynthProcessor::kArpSteps - 1, (e.getPosition().x - gridArea.getX()) / cw);
    }
    void timerCallback() override
    {
        // Motionless finger-hold: promote to velocity mode after kLongPressMs (20 Hz tick).
        if (pressMode == Pressed && pressWasOn
            && juce::Time::getMillisecondCounter() - pressDownMs > (juce::uint32) kLongPressMs)
            { pressMode = Velocity; velStep = pressStep; }
        repaint();
    }

    VASynthProcessor& proc;
    std::unique_ptr<PowerToggle> arpOn, hold;
    std::unique_ptr<HSelector> mode;
    std::unique_ptr<RotaryKnob> oct, gate, swing, tempo;
    juce::Rectangle<int> gridArea;
    static constexpr int kLongPressMs = 300;
    enum PressMode { Idle = 0, Pressed, Velocity };
    int pressMode = Idle, pressStep = -1, pressStartY = 0, pressStartVel = 100, velStep = -1;
    bool pressWasOn = false;
    juce::uint32 pressDownMs = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ArpBar)
};

// ---- LOOPER: FOUR fixed lanes (lane N == part N), each with its OWN transport ----
// Lane N records + plays part N regardless of the edit/play focus (#47). Each lane row:
// part label, REC, PLAY, MIDI/AUDIO, a per-lane BARS length knob, quantize, CLEAR, and a
// content/playhead strip. J2: each lane picks its OWN length (1..32 bars); at slow tempos a
// lane's AUDIO mode is capped to what the ring can hold (honest, shown on the row).

// ---- J3: one scene slot button. Tap = launch (quantized); long-press = copy/clear menu.
// Visual: outline = empty, filled = has content, blinking = pending (armed), solid = active.
class SceneButton : public juce::Component,
                    public  juce::SettableTooltipClient,
                    private juce::Timer
{
public:
    SceneButton (VASynthProcessor& p, int idx) : proc (p), index (idx)
    {
        setWantsKeyboardFocus (false);
        setTooltip ("Scene " + juce::String (idx + 1) + " - tap to launch, long-press to copy/clear");
    }
    void mouseDown (const juce::MouseEvent&) override { longPressed = false; startTimer (450); }
    void mouseUp   (const juce::MouseEvent&) override { stopTimer(); if (! longPressed) proc.launchScene (index); }
    void timerCallback() override
    {
        stopTimer(); longPressed = true;
        juce::PopupMenu m;
        m.addItem (1, "Copy active scene here");
        m.addItem (2, "Clear scene");
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
            [this] (int r) { if (r == 1) proc.copyActiveSceneTo (index); else if (r == 2) proc.clearScene (index); });
    }
    void paint (juce::Graphics& g) override
    {
        const bool active  = proc.activeScene() == index;
        const bool pending = proc.pendingSceneIndex() == index;
        const bool has     = proc.sceneHasContent (index);
        const auto accent  = VASynthLookAndFeel::accent();
        auto r = getLocalBounds().toFloat().reduced (1.0f);

        juce::Colour fill = active ? accent
                          : has    ? VASynthLookAndFeel::track().brighter (0.18f)
                                   : VASynthLookAndFeel::track();
        if (pending)   // blink between dim + accent while armed
        {
            const float b = 0.5f + 0.5f * (float) std::sin (juce::Time::getMillisecondCounter() * 0.012);
            fill = accent.withAlpha (0.30f + 0.55f * b);
        }
        g.setColour (fill);
        g.fillRoundedRectangle (r, 3.0f);
        if (! has && ! active)          // empty slot: just an outline
        { g.setColour (VASynthLookAndFeel::dim()); g.drawRoundedRectangle (r, 3.0f, 1.0f); }

        g.setColour (active ? juce::Colours::black : VASynthLookAndFeel::ink());
        g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
        g.drawText (juce::String (index + 1), getLocalBounds(), juce::Justification::centred, false);
    }
private:
    VASynthProcessor& proc; int index; bool longPressed = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SceneButton)
};

class LooperPanel : public juce::Component,
                    private juce::Timer
{
public:
    static constexpr int kLanes = 4;

    explicit LooperPanel (VASynthProcessor& p) : proc (p)
    {
        setWantsKeyboardFocus (false);
        const char* const recIds[]  { ParamID::loopRec,  ParamID::loopRec2,  ParamID::loopRec3,  ParamID::loopRec4 };
        const char* const playIds[] { ParamID::loopPlay, ParamID::loopPlay2, ParamID::loopPlay3, ParamID::loopPlay4 };
        const char* const modeIds[] { ParamID::loopMode, ParamID::loopMode2, ParamID::loopMode3, ParamID::loopMode4 };
        const char* const barsIds[] { ParamID::loopBars, ParamID::loopBars2, ParamID::loopBars3, ParamID::loopBars4 };
        const char* const quantIds[]{ ParamID::loopQuant, ParamID::loopQuant2, ParamID::loopQuant3, ParamID::loopQuant4 };

        auto styleBtn = [] (juce::TextButton& b)
        {
            b.setWantsKeyboardFocus (false);
            b.setColour (juce::TextButton::buttonColourId, VASynthLookAndFeel::track());
            b.setColour (juce::TextButton::textColourOffId, VASynthLookAndFeel::ink());
        };

        for (int i = 0; i < kLanes; ++i)
        {
            recBtn[(std::size_t) i]  = std::make_unique<PowerToggle> (p.apvts, recIds[(std::size_t) i],  "R");
            playBtn[(std::size_t) i] = std::make_unique<PowerToggle> (p.apvts, playIds[(std::size_t) i], "P");
            modeSel[(std::size_t) i] = std::make_unique<HSelector> (p.apvts, modeIds[(std::size_t) i], p.getMidiLearn(),
                                                                    juce::StringArray { "MIDI", "AUD" });
            barsSel[(std::size_t) i] = std::make_unique<RotaryKnob> (p.apvts, barsIds[(std::size_t) i], "BARS", p.getMidiLearn());   // per-lane length knob
            quantBtn[(std::size_t) i] = std::make_unique<PowerToggle> (p.apvts, quantIds[(std::size_t) i], "Q");   // 1/32 quantize
            addAndMakeVisible (*recBtn[(std::size_t) i]); addAndMakeVisible (*playBtn[(std::size_t) i]);
            addAndMakeVisible (*modeSel[(std::size_t) i]); addAndMakeVisible (*barsSel[(std::size_t) i]);
            addAndMakeVisible (*quantBtn[(std::size_t) i]);
            auto& cb = clearBtn[(std::size_t) i];
            cb.setButtonText ("x"); styleBtn (cb);
            cb.setTooltip ("Clear this lane (MIDI + audio)");
            cb.onClick = [this, i] { proc.clearLoopLane (i); };
            addAndMakeVisible (cb);
        }

        // J2: length is now PER-LANE (a BARS selector on each row) — no shared top-bar selector.
        expo.setButtonText ("MIDI"); styleBtn (expo);   expo.setTooltip ("Export recorded loops to a MIDI file");   expo.onClick    = [this] { exportMidi(); }; addAndMakeVisible (expo);
        expoWav.setButtonText ("WAV"); styleBtn (expoWav); expoWav.setTooltip ("Export the audio loops to a WAV file"); expoWav.onClick = [this] { exportWav(); }; addAndMakeVisible (expoWav);

        // J3: eight scene slots + the launch-quantum selector, in the top bar.
        for (int i = 0; i < VASynthProcessor::kScenes; ++i)
        { sceneBtn[(std::size_t) i] = std::make_unique<SceneButton> (p, i); addAndMakeVisible (*sceneBtn[(std::size_t) i]); }
        sceneQuant = std::make_unique<HSelector> (p.apvts, ParamID::sceneQuant, p.getMidiLearn(),
                                                  juce::StringArray { "1", "2", "4", "8", "END" });   // launch quantum (bars / loop-end)
        addAndMakeVisible (*sceneQuant);

        startTimerHz (20);   // playhead + armed-record blink
    }

    void paint (juce::Graphics& g) override
    {
        const auto tLoop = juce::Colour (0xffca6bd0);
        chrome::section (g, getLocalBounds(), "Looper  -  4 lanes (P1-P4), MIDI + audio", tLoop);

        const int maxAudioBars = proc.maxAudioLoopBars();   // honest audio cap at this tempo
        for (int i = 0; i < kLanes; ++i)
        {
            auto lane = laneStrip[(std::size_t) i];
            if (lane.isEmpty()) continue;
            const float ph = proc.loopPlayhead (i);         // J2: each lane has its own phase

            const bool audioMode = proc.apvts.getRawParameterValue (laneModeId (i))->load() > 0.5f;
            const bool hasMidi   = proc.loopLaneHasContent (i);
            const bool hasAudio  = proc.loopAudioHasContent (i);
            const int  rs        = proc.loopRecDisplayState (i);   // 0 idle,1 armed,2 rec

            g.setColour (VASynthLookAndFeel::track());
            g.fillRoundedRectangle (lane.toFloat(), 3.0f);

            // content fill (MIDI hue vs audio hue), for the lane the mode HEARS.
            const bool has = audioMode ? hasAudio : hasMidi;
            if (has)
            {
                const auto fill = audioMode ? juce::Colour (0xff58c0a8) : tLoop;
                g.setColour (fill.withAlpha (0.34f));
                g.fillRoundedRectangle (lane.reduced (2, 2).toFloat(), 3.0f);
            }
            // armed/recording pip
            if (rs > 0)
            {
                const bool recOn = rs == 2;
                const float blink = 0.5f + 0.5f * (float) std::sin (juce::Time::getMillisecondCounter() * 0.008);
                g.setColour (juce::Colour (0xffd8443a).withAlpha (recOn ? 1.0f : 0.35f + 0.5f * blink));
                g.fillEllipse (juce::Rectangle<float> ((float) lane.getX() + 4.0f, (float) lane.getCentreY() - 3.5f, 7.0f, 7.0f));
            }
            // honest audio-cap note (this lane is AUDIO but the grid can't fit in the ring).
            static const int barsForSel[] { 1, 2, 4, 8, 16, 32 };
            const int barsIdx = juce::jlimit (0, 5, (int) proc.apvts.getRawParameterValue (laneBarsId (i))->load());
            const int selBars = barsForSel[barsIdx];
            if (audioMode && selBars > maxAudioBars)
            {
                g.setColour (juce::Colour (0xffe0b050));
                g.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::bold)));
                g.drawText ("aud " + juce::String (maxAudioBars) + "b", lane.reduced (14, 0), juce::Justification::centredLeft, false);
            }
            const int px = lane.getX() + juce::roundToInt (ph * lane.getWidth());
            g.setColour (tLoop.brighter (0.3f));
            g.fillRect (juce::Rectangle<int> (px - 1, lane.getY() + 2, 2, lane.getHeight() - 4));
        }
    }

    void resized() override
    {
        auto c = chrome::sectionContent (getLocalBounds());
        // Top bar: [SCENE 1..8]  [launch quantum]  ......  [MIDI][WAV]. Taller + wider than the lane
        // rows so the scenes read as the primary control (the rows below give up ~10% of their height).
        auto top = c.removeFromTop (42); c.removeFromTop (5);
        auto exports = top.removeFromRight (98); exports.removeFromTop (exports.getHeight() / 2 - 12);
        expoWav.setBounds (exports.removeFromRight (46).reduced (2, 2)); exports.removeFromRight (3);
        expo.setBounds    (exports.removeFromRight (46).reduced (2, 2));
        for (int i = 0; i < VASynthProcessor::kScenes; ++i)
            sceneBtn[(std::size_t) i]->setBounds (top.removeFromLeft (40).reduced (2, 1));
        top.removeFromLeft (10);
        auto q = top; q.removeFromTop (q.getHeight() / 2 - 13);
        sceneQuant->setBounds (q.removeFromLeft (juce::jmin (155, juce::jmax (60, q.getWidth()))).reduced (2, 2));

        // Four lane rows: [P# label] [R][P] [MIDI/AUD] [BARS] [Q] [x] [content strip].
        const int rh = juce::jmax (22, (c.getHeight() - (kLanes - 1) * 4) / kLanes);
        for (int i = 0; i < kLanes; ++i)
        {
            auto row = c.removeFromTop (rh); c.removeFromTop (4);
            labelRect[(std::size_t) i] = row.removeFromLeft (34);
            recBtn[(std::size_t) i]->setBounds  (row.removeFromLeft (24).reduced (1, 2));
            playBtn[(std::size_t) i]->setBounds (row.removeFromLeft (24).reduced (1, 2));
            modeSel[(std::size_t) i]->setBounds (row.removeFromLeft (juce::jmin (56, row.getWidth() - 90)).reduced (1, 2));
            barsSel[(std::size_t) i]->setBounds (row.removeFromLeft (46).reduced (1, 2));   // compact BARS knob
            quantBtn[(std::size_t) i]->setBounds (row.removeFromLeft (22).reduced (1, 2));
            clearBtn[(std::size_t) i].setBounds (row.removeFromLeft (18).reduced (1, 3));
            row.removeFromLeft (3);
            laneStrip[(std::size_t) i] = row;
        }
    }

    // Part labels are painted over their reserved rects (after children, so they read on top).
    void paintOverChildren (juce::Graphics& g) override
    {
        for (int i = 0; i < kLanes; ++i)
        {
            g.setColour (VASynthLookAndFeel::ink());
            g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
            g.drawText ("P" + juce::String (i + 1), labelRect[(std::size_t) i].withTrimmedLeft (3),
                        juce::Justification::centredLeft, false);
        }
    }

private:
    static const char* laneModeId (int i)
    {
        switch (i) { case 1: return ParamID::loopMode2; case 2: return ParamID::loopMode3; case 3: return ParamID::loopMode4; default: return ParamID::loopMode; }
    }
    static const char* laneBarsId (int i)
    {
        switch (i) { case 1: return ParamID::loopBars2; case 2: return ParamID::loopBars3; case 3: return ParamID::loopBars4; default: return ParamID::loopBars; }
    }

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
    std::array<std::unique_ptr<PowerToggle>, kLanes> recBtn, playBtn, quantBtn;
    std::array<std::unique_ptr<HSelector>, kLanes> modeSel;
    std::array<std::unique_ptr<RotaryKnob>, kLanes> barsSel;   // J2: per-lane length knob (1..32 bars)
    std::array<juce::TextButton, kLanes> clearBtn;
    std::array<std::unique_ptr<SceneButton>, VASynthProcessor::kScenes> sceneBtn;   // J3
    std::unique_ptr<HSelector> sceneQuant;
    juce::TextButton expo, expoWav;
    std::array<juce::Rectangle<int>, kLanes> labelRect { }, laneStrip { };
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
    // J4#4/#5: seq + looper row ~20% taller (268 -> 322); looper already shares this height with the
    // sequencer. The synth centre section shrinks to fit (#6). (+20%, not +35%, so the right-hand
    // scope/EQ column still lays out cleanly; the master-EQ consolidation is a separate increment.)
    static constexpr int kChordH = 70, kArpH = 74, kGridH = 322, gap = 5;
    ChordBar chord;
    ArpBar arp;
    SeqPanel seq;
    LooperPanel looper;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BottomZones)
};
