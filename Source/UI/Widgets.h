#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "VASynthLookAndFeel.h"
#include "../MidiLearnManager.h"

// ============================================================================
// Touch-first, MIDI-learnable UI widgets bound to APVTS parameters. The APVTS
// attachment classes keep GUI <-> APVTS <-> DAW automation <-> MIDI-learn all in
// sync with no custom glue. Every widget refuses keyboard focus so the QWERTY
// computer-keyboard note input keeps working while twisting controls.
//
// MIDI-learn: right-click (mouse) or long-press (touch) any control -> arms
// MidiLearnManager for it (pulsing amber outline); the next incoming CC binds it
// and a small "CCnn" badge appears. The same gesture offers clear-mapping.
// ============================================================================

// R2 touch: pixels of finger travel to move a fader/knob across its FULL range
// (JUCE's Slider default is 250). Higher = less sensitive, so small movements don't
// over-shoot values during live play. Applied consistently to every fader + knob.
// ONE knob to re-tune after the layout rebuild changes control sizes (larger controls
// want more travel). ~313 = JUCE's 250 x 1.25 (~25% more travel / ~20% less per pixel).
inline constexpr int kDragPixelsForFullRange = 313;

// Clean, arm's-length-legible value readout for a float parameter: sensible
// decimal places for the magnitude, plus the parameter's own unit label (Hz, s,
// ms, ct) — JUCE's getCurrentValueAsText() shows neither (it dumps full float
// precision like "299.9999695" and never appends the label). Purely cosmetic;
// the parameter value/state is untouched.
inline juce::String formatParamValue (juce::RangedAudioParameter* p)
{
    if (auto* fp = dynamic_cast<juce::AudioParameterFloat*> (p))
    {
        const float v = fp->get();
        const float a = std::abs (v);
        juce::String num = a >= 100.0f ? juce::String (juce::roundToInt (v))
                         : a >= 10.0f  ? juce::String (v, 1)
                         : a >= 1.0f   ? juce::String (v, 2)
                                       : juce::String (v, 3);
        const juce::String unit = fp->getLabel();
        return unit.isEmpty() ? num : num + " " + unit;
    }
    return p != nullptr ? p->getCurrentValueAsText() : juce::String();
}

// Shared MIDI-learn interaction for a control bound to one parameter.
class LearnableComponent : public juce::Component,
                           private juce::Timer
{
public:
    LearnableComponent (MidiLearnManager& learnToUse, juce::String paramIdToUse)
        : learn (learnToUse), paramID (std::move (paramIdToUse))
    {
        setWantsKeyboardFocus (false);
    }

    // Call from a subclass ctor after the inner control is added, passing it so
    // long-press/right-click on the control is captured here too.
    void listenForLearnGestures (juce::Component& inner)
    {
        inner.addMouseListener (this, true);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())          { showLearnMenu(); return; }
        pressStart = juce::Time::getMillisecondCounter();
        longPressArmed = true;
        startTimer (60);                   // poll for the long-press threshold
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.getDistanceFromDragStart() > 8) longPressArmed = false;   // it's a value drag
    }
    void mouseUp (const juce::MouseEvent&) override { longPressArmed = false; }

    // Subclasses call this in paint() to draw the armed outline + CC badge.
    void paintLearnDecorations (juce::Graphics& g)
    {
        const int cc = learn.getCCForParam (paramID);
        if (cc >= 0)
        {
            const int bw = juce::jmin (getWidth() - 2, 34);
            auto badge = getLocalBounds().removeFromTop (12).removeFromRight (bw).toFloat().reduced (0.5f);
            g.setColour (VASynthLookAndFeel::accent().withAlpha (0.92f));
            g.fillRoundedRectangle (badge, 2.5f);
            g.setColour (juce::Colours::black);
            g.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::bold)));
            g.drawText ("CC" + juce::String (cc), badge, juce::Justification::centred, false);
        }
        if (learn.isLearningParam (paramID))
        {
            const float a = 0.35f + 0.45f * (float) std::abs (std::sin (juce::Time::getMillisecondCounter() * 0.006));
            g.setColour (VASynthLookAndFeel::accentWarm().withAlpha (a));
            g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.5f), 5.0f, 2.5f);
        }
    }

    const juce::String& parameterID() const { return paramID; }

private:
    void timerCallback() override
    {
        if (longPressArmed && juce::Time::getMillisecondCounter() - pressStart > 500)
        {
            longPressArmed = false;
            armLearn();
        }
        if (learn.isLearningParam (paramID)) repaint();   // pulse while armed
        else if (! longPressArmed) stopTimer();
    }

    void armLearn()
    {
        learn.armLearn (paramID);
        startTimer (33);                   // ~30 Hz pulse while armed
        repaint();
    }

    void showLearnMenu()
    {
        juce::PopupMenu m;
        m.addItem (1, "MIDI-learn this control");
        const int cc = learn.getCCForParam (paramID);
        if (cc >= 0) m.addItem (2, "Clear mapping (CC" + juce::String (cc) + ")");
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                         [this] (int r)
                         {
                             if (r == 1) armLearn();
                             else if (r == 2) { learn.clearParam (paramID); repaint(); }
                         });
    }

    MidiLearnManager& learn;
    juce::String paramID;
    juce::uint32 pressStart = 0;
    bool longPressArmed = false;
};

// ---------------------------------------------------------------------------
// Hardware-style on/off kill switch bound to a bool parameter (lit = on).
class PowerToggle : public juce::Component
{
public:
    PowerToggle (juce::AudioProcessorValueTreeState& apvts, const juce::String& pid, juce::String label)
        : name (std::move (label))
    {
        btn.setClickingTogglesState (true);
        btn.setWantsKeyboardFocus (false);
        btn.setButtonText (name);
        btn.setColour (juce::TextButton::buttonColourId,   VASynthLookAndFeel::track());
        btn.setColour (juce::TextButton::buttonOnColourId, VASynthLookAndFeel::accent());
        btn.setColour (juce::TextButton::textColourOnId,   juce::Colours::black);
        btn.setColour (juce::TextButton::textColourOffId,  VASynthLookAndFeel::dim());
        addAndMakeVisible (btn);
        attachment = std::make_unique<juce::ButtonParameterAttachment> (*apvts.getParameter (pid), btn);
        getProperties().set ("layoutFlex", 0.55);
    }
    void resized() override { btn.setBounds (getLocalBounds().reduced (2)); }

private:
    juce::String name;
    juce::TextButton btn;
    std::unique_ptr<juce::ButtonParameterAttachment> attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PowerToggle)
};

// ---------------------------------------------------------------------------
// Vertical fader + name + live value readout, bound to a float parameter.
class LabelledFader : public LearnableComponent
{
public:
    // emphasis = a prominent fader (e.g. MASTER): larger, higher-contrast label,
    // an accent frame, and a bright value readout so it stands out in its section.
    LabelledFader (juce::AudioProcessorValueTreeState& apvts, const juce::String& pid,
                   juce::String displayName, MidiLearnManager& learnMgr, bool emphasise = false)
        : LearnableComponent (learnMgr, pid), name (std::move (displayName)), emphasis (emphasise)
    {
        slider.setSliderStyle (juce::Slider::LinearVertical);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setVelocityBasedMode (false);           // absolute drag distance, not velocity
        slider.setSliderSnapsToMousePosition (false);  // R2 GRAB mode: first touch acquires the
                                                       // control with NO value change; the value
                                                       // moves only on drag, relative to the grab
                                                       // point (snapping caused live-perf surprises).
        slider.setMouseDragSensitivity (kDragPixelsForFullRange);   // R2: gentler drag-to-value
        slider.setWantsKeyboardFocus (false);
        addAndMakeVisible (slider);
        attachment = std::make_unique<juce::SliderParameterAttachment> (*apvts.getParameter (pid), slider);

        param = apvts.getParameter (pid);
        listenForLearnGestures (slider);
    }

    void paint (juce::Graphics& g) override
    {
        if (emphasis)
        {
            // Distinct rounded frame so MASTER reads as the section's headline.
            auto r = getLocalBounds().toFloat().reduced (1.5f);
            g.setColour (VASynthLookAndFeel::accentWarm().withAlpha (0.10f));
            g.fillRoundedRectangle (r, 6.0f);
            g.setColour (VASynthLookAndFeel::accentWarm().withAlpha (0.75f));
            g.drawRoundedRectangle (r, 6.0f, 1.6f);
        }

        // Name: high-contrast (near-white / amber for master), larger for arm's-
        // length reading. Bold so it stays legible on a busy dark panel.
        g.setColour (emphasis ? VASynthLookAndFeel::accentWarm() : VASynthLookAndFeel::ink());
        g.setFont (juce::Font (juce::FontOptions (emphasis ? 15.0f : 13.0f, juce::Font::bold)));
        g.drawFittedText (emphasis ? name.toUpperCase() : name,
                          getLocalBounds().removeFromTop (labelH()), juce::Justification::centred, 1);

        // Live value readout with the parameter's own units/text (auto-fit width),
        // in the accent colour so the current setting is easy to read at a glance.
        g.setColour (emphasis ? VASynthLookAndFeel::ink() : VASynthLookAndFeel::accent());
        g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                                  emphasis ? 14.0f : 12.5f, juce::Font::bold)));
        const juce::String text = formatParamValue (param);
        g.drawFittedText (text, getLocalBounds().removeFromBottom (labelH()), juce::Justification::centred, 1);

        paintLearnDecorations (g);
    }

    void resized() override
    {
        const int pad = emphasis ? 6 : 2;
        slider.setBounds (getLocalBounds().reduced (pad, 0).withTrimmedTop (labelH() + 2).withTrimmedBottom (labelH() + 2));
    }

private:
    int labelH() const { return emphasis ? 20 : 18; }

    juce::String name;
    bool emphasis = false;
    juce::Slider slider;
    juce::RangedAudioParameter* param = nullptr;
    std::unique_ptr<juce::SliderParameterAttachment> attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LabelledFader)
};

// ---------------------------------------------------------------------------
// Transient toast notification (e.g. "Launchkey Mini connected"). Holds for ~2 s
// then fades. Intercepts no input and refuses focus, so it never disturbs the
// controls beneath it or the QWERTY note input.
class Toast : public juce::Component,
              private juce::Timer
{
public:
    Toast()
    {
        setInterceptsMouseClicks (false, false);
        setWantsKeyboardFocus (false);
        setVisible (false);
    }

    void show (const juce::String& message)
    {
        text = message;
        ticks = 0;
        alpha = 1.0f;
        setVisible (true);
        toFront (false);
        startTimerHz (30);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (VASynthLookAndFeel::panelLight().withAlpha (alpha * 0.96f));
        g.fillRoundedRectangle (r, 8.0f);
        g.setColour (VASynthLookAndFeel::accent().withAlpha (alpha));
        g.drawRoundedRectangle (r.reduced (0.75f), 8.0f, 1.4f);
        g.setColour (VASynthLookAndFeel::ink().withAlpha (alpha));
        g.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
        g.drawFittedText (text, getLocalBounds().reduced (14, 0), juce::Justification::centred, 1);
    }

private:
    void timerCallback() override
    {
        ++ticks;
        if (ticks > 60)                                   // ~2 s hold, then ~0.5 s fade
            alpha = juce::jmax (0.0f, 1.0f - (float) (ticks - 60) / 15.0f);
        if (alpha <= 0.0f) { setVisible (false); stopTimer(); }
        repaint();
    }

    juce::String text;
    float alpha = 1.0f;
    int   ticks = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Toast)
};

// ---------------------------------------------------------------------------
// Rotary knob + name + live value, bound to a float parameter. MIDI-learnable and
// focus-refusing like the faders; used in the FX panel where knobs read better
// than a wall of faders.
class RotaryKnob : public LearnableComponent
{
public:
    // sideLabel: knob on the LEFT (square) with the name + value stacked to its
    // right — for wide/short rows (e.g. the filter's vertical knob column) where a
    // name-above/value-below stack would leave the row half-empty.
    RotaryKnob (juce::AudioProcessorValueTreeState& apvts, const juce::String& pid,
                juce::String displayName, MidiLearnManager& learnMgr, bool sideLabelLayout = false)
        : LearnableComponent (learnMgr, pid), name (std::move (displayName)), sideLabel (sideLabelLayout)
    {
        slider.setSliderStyle (juce::Slider::RotaryVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setVelocityBasedMode (false);           // relative drag distance (grab mode)
        slider.setSliderSnapsToMousePosition (false);  // R2: acquire on touch, no value jump
        slider.setMouseDragSensitivity (kDragPixelsForFullRange);   // R2: gentler drag-to-value
        slider.setWantsKeyboardFocus (false);
        addAndMakeVisible (slider);
        attachment = std::make_unique<juce::SliderParameterAttachment> (*apvts.getParameter (pid), slider);

        param = apvts.getParameter (pid);
        listenForLearnGestures (slider);
    }

    void paint (juce::Graphics& g) override
    {
        const juce::String text = formatParamValue (param);
        if (sideLabel)
        {
            auto lab = getLocalBounds().withTrimmedLeft (getHeight() + 4);
            g.setColour (VASynthLookAndFeel::ink());
            g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
            g.drawFittedText (name, lab.removeFromTop (lab.getHeight() * 3 / 5), juce::Justification::centredLeft, 1);
            g.setColour (VASynthLookAndFeel::accent());
            g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 11.5f, juce::Font::bold)));
            g.drawFittedText (text, lab, juce::Justification::centredLeft, 1);
            paintLearnDecorations (g);
            return;
        }

        g.setColour (VASynthLookAndFeel::ink());
        g.setFont (juce::Font (juce::FontOptions (12.5f, juce::Font::bold)));
        g.drawFittedText (name, getLocalBounds().removeFromTop (16), juce::Justification::centred, 1);

        g.setColour (VASynthLookAndFeel::accent());
        g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::bold)));
        g.drawFittedText (text, getLocalBounds().removeFromBottom (15), juce::Justification::centred, 1);

        paintLearnDecorations (g);
    }

    void resized() override
    {
        if (sideLabel)
            slider.setBounds (getLocalBounds().removeFromLeft (getHeight()));
        else
            slider.setBounds (getLocalBounds().withTrimmedTop (17).withTrimmedBottom (16));
    }

private:
    juce::String name;
    bool sideLabel = false;
    juce::Slider slider;
    juce::RangedAudioParameter* param = nullptr;
    std::unique_ptr<juce::SliderParameterAttachment> attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RotaryKnob)
};

// ---------------------------------------------------------------------------
// Touch-friendly segmented button row for a choice parameter (one tap, visible
// state) — preferred over a dropdown where the option count allows.
class SegmentedControl : public LearnableComponent
{
public:
    SegmentedControl (juce::AudioProcessorValueTreeState& apvts, const juce::String& pid,
                      juce::String displayName, MidiLearnManager& learnMgr)
        : LearnableComponent (learnMgr, pid), name (std::move (displayName))
    {
        choice = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (pid));
        jassert (choice != nullptr);

        // Segmented controls need more width than a single fader so labels read.
        getProperties().set ("layoutFlex", juce::jmax (2.2, 0.85 * choice->choices.size()));

        for (int i = 0; i < choice->choices.size(); ++i)
        {
            auto* b = buttons.add (new juce::TextButton (choice->choices[i]));
            b->setClickingTogglesState (false);
            b->setWantsKeyboardFocus (false);
            b->setColour (juce::TextButton::buttonColourId, VASynthLookAndFeel::track());
            b->setColour (juce::TextButton::buttonOnColourId, VASynthLookAndFeel::accent());
            b->setColour (juce::TextButton::textColourOffId, VASynthLookAndFeel::dim());
            b->setColour (juce::TextButton::textColourOnId, juce::Colours::black);
            const int idx = i;
            b->onClick = [this, idx] { setIndex (idx); };
            addAndMakeVisible (b);
            listenForLearnGestures (*b);
        }
        attachment = std::make_unique<juce::ParameterAttachment> (
            *choice, [this] (float) { refresh(); }, nullptr);
        attachment->sendInitialUpdate();
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (VASynthLookAndFeel::ink());
        g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
        g.drawText (name, getLocalBounds().removeFromTop (18), juce::Justification::centred, false);
        paintLearnDecorations (g);
    }

    void resized() override
    {
        // Stack buttons vertically — full-width finger targets filling the column.
        auto r = getLocalBounds().withTrimmedTop (18);
        const int n = buttons.size();
        if (n == 0) return;
        const int h = r.getHeight() / n;
        for (int i = 0; i < n; ++i)
            buttons[i]->setBounds (r.removeFromTop (i == n - 1 ? r.getHeight() : h).reduced (2, 2));
    }

private:
    void setIndex (int i)
    {
        choice->beginChangeGesture();
        choice->setValueNotifyingHost (choice->convertTo0to1 ((float) i));
        choice->endChangeGesture();
        refresh();
    }
    void refresh()
    {
        const int current = choice->getIndex();
        for (int i = 0; i < buttons.size(); ++i)
            buttons[i]->setToggleState (i == current, juce::dontSendNotification);
    }

    juce::String name;
    juce::AudioParameterChoice* choice = nullptr;
    juce::OwnedArray<juce::TextButton> buttons;
    std::unique_ptr<juce::ParameterAttachment> attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SegmentedControl)
};

// ---------------------------------------------------------------------------
// Horizontal one-tap selector for a choice parameter — headerless (the section
// header names it), buttons spread left-to-right. This is the R2 layout's segmented
// grammar (osc wave, LFO dest, filter type, chord degree/quality). Optional short
// labels override the parameter's long choice names ("Square" -> "SQR"). Optionally
// draws its own tinted labels above/beside (headerless by default). MIDI-learnable
// and focus-refusing like every control.
class HSelector : public LearnableComponent
{
public:
    HSelector (juce::AudioProcessorValueTreeState& apvts, const juce::String& pid,
               MidiLearnManager& learnMgr, juce::StringArray labelOverride = {})
        : LearnableComponent (learnMgr, pid)
    {
        choice = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (pid));
        jassert (choice != nullptr);
        labels = labelOverride.isEmpty() ? choice->choices : labelOverride;

        for (int i = 0; i < choice->choices.size(); ++i)
        {
            auto* b = buttons.add (new juce::TextButton (labels[juce::jmin (i, labels.size() - 1)]));
            b->setClickingTogglesState (false);
            b->setWantsKeyboardFocus (false);
            b->setColour (juce::TextButton::buttonColourId, VASynthLookAndFeel::track());
            b->setColour (juce::TextButton::buttonOnColourId, VASynthLookAndFeel::accent());
            b->setColour (juce::TextButton::textColourOffId, VASynthLookAndFeel::ink());
            b->setColour (juce::TextButton::textColourOnId, juce::Colours::black);
            const int idx = i;
            b->onClick = [this, idx] { setIndex (idx); };
            addAndMakeVisible (b);
            listenForLearnGestures (*b);
        }
        attachment = std::make_unique<juce::ParameterAttachment> (
            *choice, [this] (float) { refresh(); }, nullptr);
        attachment->sendInitialUpdate();
    }

    void paint (juce::Graphics& g) override { paintLearnDecorations (g); }

    void resized() override
    {
        auto r = getLocalBounds();
        const int n = buttons.size();
        if (n == 0) return;
        for (int i = 0; i < n; ++i)
        {
            auto cell = juce::Rectangle<int> (r.getX() + i * r.getWidth() / n, r.getY(),
                                              r.getWidth() / n, r.getHeight());
            buttons[i]->setBounds (cell.reduced (2));
        }
    }

private:
    void setIndex (int i)
    {
        choice->beginChangeGesture();
        choice->setValueNotifyingHost (choice->convertTo0to1 ((float) i));
        choice->endChangeGesture();
        refresh();
    }
    void refresh()
    {
        const int current = choice->getIndex();
        for (int i = 0; i < buttons.size(); ++i)
            buttons[i]->setToggleState (i == current, juce::dontSendNotification);
    }

    juce::AudioParameterChoice* choice = nullptr;
    juce::StringArray labels;
    juce::OwnedArray<juce::TextButton> buttons;
    std::unique_ptr<juce::ParameterAttachment> attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HSelector)
};

// ---------------------------------------------------------------------------
// LFO shape picker drawn as four stacked waveform ICONS (Triangle / Sine / Square /
// S&H) bound to a choice parameter — the R2 LFO grammar. Tap an icon to select.
// MIDI-learnable + focus-refusing.
class ShapeSelector : public LearnableComponent
{
public:
    ShapeSelector (juce::AudioProcessorValueTreeState& apvts, const juce::String& pid,
                   MidiLearnManager& learnMgr)
        : LearnableComponent (learnMgr, pid)
    {
        choice = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (pid));
        jassert (choice != nullptr);
        attachment = std::make_unique<juce::ParameterAttachment> (
            *choice, [this] (float) { repaint(); }, nullptr);
        attachment->sendInitialUpdate();
    }

    // Draw one waveform icon (0 Tri, 1 Sin, 2 Sqr, 3 S&H) filling r.
    static void drawIcon (juce::Graphics& g, juce::Rectangle<int> r, int kind, bool on)
    {
        g.setColour (on ? VASynthLookAndFeel::accent() : VASynthLookAndFeel::track());
        g.fillRoundedRectangle (r.toFloat(), 4.0f);
        auto a = r.toFloat().reduced (r.getWidth() * 0.18f, r.getHeight() * 0.28f);
        const float x0 = a.getX(), w = a.getWidth(), y0 = a.getCentreY(), h = a.getHeight() * 0.5f;
        g.setColour (on ? juce::Colour (0xff0e1319) : VASynthLookAndFeel::ink());
        juce::Path p;
        if (kind == 0)      { p.startNewSubPath (x0, y0); p.lineTo (x0+w*0.25f, y0-h); p.lineTo (x0+w*0.75f, y0+h); p.lineTo (x0+w, y0); }
        else if (kind == 1) { p.startNewSubPath (x0, y0); for (int i = 1; i <= 20; ++i) { float t = i/20.0f; p.lineTo (x0+w*t, y0 - std::sin (t*6.283f)*h); } }
        else if (kind == 2) { p.startNewSubPath (x0, y0+h); p.lineTo (x0, y0-h); p.lineTo (x0+w*0.5f, y0-h); p.lineTo (x0+w*0.5f, y0+h); p.lineTo (x0+w, y0+h); p.lineTo (x0+w, y0-h); }
        else                { const float s[5] { 0.3f,-0.6f,0.5f,-0.2f,0.7f }; float px = x0; for (int i = 0; i < 5; ++i) { float ny = y0 - s[i]*h; p.startNewSubPath (px, ny); p.lineTo (px+w/5.0f, ny); px += w/5.0f; } }
        g.strokePath (p, juce::PathStrokeType (1.6f));
    }

    void paint (juce::Graphics& g) override
    {
        const int cur = choice != nullptr ? choice->getIndex() : 0;
        const int n = 4;
        const int ih = (getHeight() - (n - 1) * gap) / n;
        for (int k = 0; k < n; ++k)
        {
            cells[(std::size_t) k] = juce::Rectangle<int> (0, k * (ih + gap), getWidth(), ih);
            drawIcon (g, cells[(std::size_t) k], k, k == cur);
        }
        paintLearnDecorations (g);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        LearnableComponent::mouseUp (e);
        if (e.mods.isPopupMenu() || e.getDistanceFromDragStart() > 8) return;
        for (int k = 0; k < 4; ++k)
            if (cells[(std::size_t) k].contains (e.getPosition()) && choice != nullptr)
            {
                choice->beginChangeGesture();
                choice->setValueNotifyingHost (choice->convertTo0to1 ((float) k));
                choice->endChangeGesture();
                repaint();
                return;
            }
    }

private:
    static constexpr int gap = 3;
    juce::AudioParameterChoice* choice = nullptr;
    std::array<juce::Rectangle<int>, 4> cells {};
    std::unique_ptr<juce::ParameterAttachment> attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ShapeSelector)
};
