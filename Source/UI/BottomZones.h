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
        enable->setBounds (c.removeFromLeft (78).reduced (2)); c.removeFromLeft (6);
        root->setBounds  (c.removeFromLeft (juce::jmin (300, c.getWidth() / 2))); c.removeFromLeft (6);
        scale->setBounds (c.removeFromLeft (110)); c.removeFromLeft (8);
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

// ---- a collapsible placeholder zone (rhythm / looper; engines land in R3) ---
class CollapZone : public juce::Component
{
public:
    CollapZone (juce::String titleText, juce::Colour tintColour, juce::String note)
        : title (std::move (titleText)), tint (tintColour), blurb (std::move (note))
    {
        setWantsKeyboardFocus (false);
    }

    std::function<void()> onToggle;
    bool expanded = false;
    static constexpr int kBarH = 32, kExpandedH = 150;
    int preferredHeight() const { return expanded ? kExpandedH : kBarH; }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        auto bar = r.removeFromTop (kBarH);
        g.setColour (tint); g.fillRoundedRectangle (bar.toFloat(), 5.0f);
        g.fillRect (bar.withTop (bar.getCentreY()));
        g.setColour (chrome::onTint());
        g.setFont (juce::Font (juce::FontOptions (12.5f, juce::Font::bold)));
        g.drawText ((expanded ? "  v   " : "  >   ") + title.toUpperCase(), bar, juce::Justification::centredLeft, false);

        if (expanded)
        {
            g.setColour (VASynthLookAndFeel::panelLight().darker (0.12f));
            g.fillRoundedRectangle (r.toFloat(), 5.0f);
            g.setColour (VASynthLookAndFeel::dim());
            g.setFont (juce::Font (juce::FontOptions (13.0f)));
            g.drawText (blurb, r.reduced (16, 0), juce::Justification::centred, false);
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.getDistanceFromDragStart() > 8) return;
        if (e.getPosition().y < kBarH) { expanded = ! expanded; if (onToggle) onToggle(); }
    }

private:
    juce::String title;
    juce::Colour tint;
    juce::String blurb;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CollapZone)
};

// ---- the whole bottom workstation ------------------------------------------
class BottomZones : public juce::Component
{
public:
    explicit BottomZones (VASynthProcessor& p)
        : chord (p),
          rhythm ("Rhythm  -  arp + sequencer",  juce::Colour (0xffe0b13a), "16-step arp + step sequencer per part  -  arrives in R3"),
          looper ("Looper  -  per-part MIDI loops + session export", juce::Colour (0xffca6bd0), "record + overdub MIDI loops per part, export stems + MIDI  -  arrives in R3")
    {
        setWantsKeyboardFocus (false);
        addAndMakeVisible (chord);
        addAndMakeVisible (rhythm);
        addAndMakeVisible (looper);
        rhythm.onToggle = [this] { relayout(); };
        looper.onToggle = [this] { relayout(); };
    }

    // Editor calls this to size the bottom band; it changes as zones expand.
    int preferredHeight() const
    {
        return kChordH + gap + rhythm.preferredHeight() + gap + looper.preferredHeight();
    }

    std::function<void()> onResizeNeeded;

    void resized() override
    {
        auto r = getLocalBounds();
        chord.setBounds (r.removeFromTop (kChordH)); r.removeFromTop (gap);
        rhythm.setBounds (r.removeFromTop (rhythm.preferredHeight())); r.removeFromTop (gap);
        looper.setBounds (r.removeFromTop (looper.preferredHeight()));
    }

private:
    void relayout() { if (onResizeNeeded) onResizeNeeded(); else resized(); }

    static constexpr int kChordH = 46, gap = 5;
    ChordBar chord;
    CollapZone rhythm, looper;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BottomZones)
};
