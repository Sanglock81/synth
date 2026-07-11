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

// ---- static paint helpers for the (non-functional) rhythm/looper previews ---
namespace bottomdraw
{
    inline void selector (juce::Graphics& g, juce::Rectangle<int> r, juce::StringArray opts, int sel)
    {
        const int n = juce::jmax (1, opts.size());
        for (int i = 0; i < opts.size(); ++i)
        {
            auto cell = juce::Rectangle<int> (r.getX() + i * r.getWidth() / n, r.getY(), r.getWidth() / n, r.getHeight()).reduced (2);
            g.setColour (i == sel ? VASynthLookAndFeel::accent() : VASynthLookAndFeel::track());
            g.fillRoundedRectangle (cell.toFloat(), 4.0f);
            g.setColour (i == sel ? chrome::onTint() : VASynthLookAndFeel::ink());
            g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
            g.drawText (opts[i], cell, juce::Justification::centred, false);
        }
    }
    inline void toggle (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& t, bool on)
    {
        g.setColour (on ? VASynthLookAndFeel::accent() : VASynthLookAndFeel::track());
        g.fillRoundedRectangle (r.reduced (2).toFloat(), 4.0f);
        g.setColour (on ? chrome::onTint() : VASynthLookAndFeel::ink());
        g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
        g.drawText (t, r, juce::Justification::centred, false);
    }
    inline void knob (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& label, float v)
    {
        auto lab = r.removeFromBottom (13);
        const int d = juce::jmin (juce::jmin (r.getWidth() - 2, r.getHeight() - 2), 46);
        auto c = r.withSizeKeepingCentre (d, d).toFloat();
        g.setColour (VASynthLookAndFeel::track()); g.fillEllipse (c);
        const auto ctr = c.getCentre(); const float r0 = c.getWidth() * 0.5f - 3.0f;
        const float a1 = 2.30f + v * 4.66f;
        juce::Path arc; arc.addCentredArc (ctr.x, ctr.y, r0, r0, 0.0f, 2.30f, a1, true);
        g.setColour (VASynthLookAndFeel::accent()); g.strokePath (arc, juce::PathStrokeType (2.5f));
        g.setColour (VASynthLookAndFeel::ink()); g.drawLine (ctr.x, ctr.y, ctr.x + std::cos (a1 + 1.57f) * r0 * 0.8f, ctr.y + std::sin (a1 + 1.57f) * r0 * 0.8f, 2.0f);
        g.setColour (VASynthLookAndFeel::dim()); g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
        g.drawText (label, lab, juce::Justification::centred, false);
    }
    inline void previewTag (juce::Graphics& g, juce::Rectangle<int> header)
    {
        g.setColour (chrome::onTint().withAlpha (0.6f));
        g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
        g.drawText ("preview  -  R3", header.reduced (10, 0), juce::Justification::centredRight, false);
    }
}

// ---- RHYTHM preview (arp + 16-step sequencer; engine lands in R3) -----------
class RhythmPanel : public juce::Component
{
public:
    RhythmPanel() { setWantsKeyboardFocus (false); }
    void paint (juce::Graphics& g) override
    {
        const auto tRhy = juce::Colour (0xffe0b13a);
        auto r = chrome::section (g, getLocalBounds(), "Rhythm  -  arp + sequencer", tRhy);
        bottomdraw::previewTag (g, getLocalBounds().removeFromTop (chrome::kHeaderH));

        auto ctrls = r.removeFromTop (44); r.removeFromTop (6);
        bottomdraw::selector (g, ctrls.removeFromLeft (250), { "UP","DOWN","UP/DN","RAND","PLAYED" }, 0); ctrls.removeFromLeft (6);
        bottomdraw::knob (g, ctrls.removeFromLeft (58), "OCT", 0.3f); ctrls.removeFromLeft (2);
        bottomdraw::knob (g, ctrls.removeFromLeft (58), "GATE", 0.6f); ctrls.removeFromLeft (2);
        bottomdraw::knob (g, ctrls.removeFromLeft (58), "SWING", 0.5f); ctrls.removeFromLeft (8);
        bottomdraw::toggle (g, ctrls.removeFromLeft (66), "LATCH", true);
        bottomdraw::toggle (g, ctrls.removeFromLeft (66), "HOLD", false);

        const int cells = 16, cw = juce::jmax (1, r.getWidth() / cells);
        for (int s = 0; s < cells; ++s)
        {
            auto cell = juce::Rectangle<int> (r.getX() + s * cw, r.getY(), cw, r.getHeight()).reduced (2);
            const bool on = (s % 4 == 0) || s == 6 || s == 10 || s == 13;
            g.setColour (on ? tRhy : VASynthLookAndFeel::track());
            g.fillRoundedRectangle (cell.toFloat(), 3.0f);
            if (on) { g.setColour (chrome::onTint().withAlpha (0.4f)); g.fillRect (cell.removeFromBottom (cell.getHeight() * (s % 3 + 1) / 4)); }
        }
    }
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RhythmPanel)
};

// ---- LOOPER preview (per-part MIDI loops + export; engine lands in R3) ------
class LooperPanel : public juce::Component
{
public:
    LooperPanel() { setWantsKeyboardFocus (false); }
    void paint (juce::Graphics& g) override
    {
        const auto tLoop = juce::Colour (0xffca6bd0);
        auto r = chrome::section (g, getLocalBounds(), "Looper  -  per-part MIDI loops + session export", tLoop);
        bottomdraw::previewTag (g, getLocalBounds().removeFromTop (chrome::kHeaderH));

        auto bar = r.removeFromTop (40); r.removeFromTop (6);
        bottomdraw::toggle (g, bar.removeFromLeft (70), "REC", false);
        bottomdraw::toggle (g, bar.removeFromLeft (70), "PLAY", true);
        bottomdraw::toggle (g, bar.removeFromLeft (70), "CLEAR", false);
        bottomdraw::toggle (g, bar.removeFromLeft (94), "SYNC 1 bar", true);
        g.setColour (VASynthLookAndFeel::dim());
        g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
        g.drawText ("EXPORT  ->  stems + MIDI", bar, juce::Justification::centredRight, false);

        const char* lanes[] { "P1 lead", "P2 drums", "P3 pad", "P4 --" };
        const int lh = juce::jmax (1, (r.getHeight() - 3 * 5) / 4);
        for (int i = 0; i < 4; ++i)
        {
            auto lane = r.removeFromTop (lh); r.removeFromTop (5);
            g.setColour (VASynthLookAndFeel::track()); g.fillRoundedRectangle (lane.toFloat(), 4.0f);
            g.setColour (VASynthLookAndFeel::ink()); g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
            g.drawText (lanes[i], lane.removeFromLeft (70).withTrimmedLeft (8), juce::Justification::centredLeft, false);
            if (i < 3)
            {
                g.setColour (tLoop.withAlpha (0.5f)); juce::Random rr (i + 3);
                for (int x = 0; x < lane.getWidth(); x += 6)
                { float h = lane.getHeight() * (0.2f + 0.7f * rr.nextFloat());
                  g.fillRect (juce::Rectangle<float> ((float) (lane.getX() + x), lane.getCentreY() - h / 2, 3.0f, h)); }
            }
        }
    }
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LooperPanel)
};

// ---- the whole bottom workstation ------------------------------------------
class BottomZones : public juce::Component
{
public:
    explicit BottomZones (VASynthProcessor& p) : chord (p)
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
