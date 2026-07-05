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
// Vertical fader + name + live value readout, bound to a float parameter.
class LabelledFader : public LearnableComponent
{
public:
    LabelledFader (juce::AudioProcessorValueTreeState& apvts, const juce::String& pid,
                   juce::String displayName, MidiLearnManager& learnMgr)
        : LearnableComponent (learnMgr, pid), name (std::move (displayName))
    {
        slider.setSliderStyle (juce::Slider::LinearVertical);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setVelocityBasedMode (false);           // absolute-ish for predictable touch
        slider.setWantsKeyboardFocus (false);
        addAndMakeVisible (slider);
        attachment = std::make_unique<juce::SliderParameterAttachment> (*apvts.getParameter (pid), slider);

        param = apvts.getParameter (pid);
        listenForLearnGestures (slider);
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (VASynthLookAndFeel::dim());
        g.setFont (juce::Font (juce::FontOptions (11.5f)));
        g.drawText (name, getLocalBounds().removeFromTop (16), juce::Justification::centred, false);

        // live value readout with the parameter's own units/text
        g.setColour (VASynthLookAndFeel::ink());
        g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 11.5f, juce::Font::plain)));
        const juce::String text = param != nullptr ? param->getCurrentValueAsText() : juce::String();
        g.drawText (text, getLocalBounds().removeFromBottom (16), juce::Justification::centred, false);

        paintLearnDecorations (g);
    }

    void resized() override
    {
        slider.setBounds (getLocalBounds().withTrimmedTop (18).withTrimmedBottom (18));
    }

private:
    juce::String name;
    juce::Slider slider;
    juce::RangedAudioParameter* param = nullptr;
    std::unique_ptr<juce::SliderParameterAttachment> attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LabelledFader)
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
        g.setColour (VASynthLookAndFeel::dim());
        g.setFont (juce::Font (juce::FontOptions (11.5f)));
        g.drawText (name, getLocalBounds().removeFromTop (16), juce::Justification::centred, false);
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
