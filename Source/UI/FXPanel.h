#pragma once
#include "VASynthLookAndFeel.h"
#include "PanelChrome.h"
#include "Widgets.h"
#include "../PluginProcessor.h"
#include <array>
#include <vector>

// ============================================================================
// FX panel — a centre section (signal-flow order). Four effect blocks
// (SAT+WIDTH / chorus / delay / reverb), each a knob strip beneath a backlit NAME
// BAR. The bar glows in the FX tint when the effect is on and darkens when off —
// the on/off IS the label, TAP the bar to toggle. Knobs are MIDI-learnable and
// refuse keyboard focus, so QWERTY note input keeps working.
//
// REORDERING is by explicit up/down chevrons at the right of each name bar (tap
// to move the block one slot earlier/later in the chain). There is deliberately
// NO drag gesture: the chevrons are the only way to reorder, so grabbing a knob
// can never nudge a block. EQ is not one of these blocks — it is a fixed final
// stage with its own section (EQPanel), so the four blocks reorder ahead of it.
//
// The chain order is a GLOBAL, user-controlled setting. A SOUND preset does not
// rearrange it (only a preset that deliberately carries an fxOrder does); the
// order the user picks with the chevrons persists in the session state.
// ============================================================================

class FXPanel : public juce::Component,
                private juce::Timer
{
public:
    static constexpr int kNumShown = 4;   // SAT+WIDTH/chorus/delay/reverb (EQ excluded — fixed last)

    explicit FXPanel (VASynthProcessor& p) : proc (p)
    {
        syncFromProc();

        for (int i = 0; i < kNumShown; ++i)
        {
            blocks[(size_t) i] = std::make_unique<FXBlock> (*this, proc, i, defs()[(size_t) i]);
            addAndMakeVisible (*blocks[(size_t) i]);
        }
        startTimerHz (8);      // resync the visual order if the chain order changes (e.g. preset with an fxOrder)
    }

    void paint (juce::Graphics& g) override
    { chrome::section (g, getLocalBounds(), "FX", tint); }

    void resized() override { layoutBlocks(); }

    // Move the block for FX `fxIndex` one slot earlier (dir -1) or later (dir +1) in the chain,
    // then push the full 5-slot order (EQ always last) to the processor. Called by the chevrons.
    void moveBlock (int fxIndex, int dir)
    {
        const int slot = slotOf (fxIndex);
        const int ns   = slot + dir;
        if (slot < 0 || ns < 0 || ns >= kNumShown) return;
        std::swap (disp[slot], disp[ns]);

        int full[5];
        for (int i = 0; i < kNumShown; ++i) full[i] = disp[i];
        full[kNumShown] = FXChain::EQ_;   // EQ always runs last
        proc.setFxOrder (full);

        layoutBlocks();
        repaint();
        for (auto& b : blocks) if (b) b->repaint();   // refresh chevron enabled-state on every block
    }

    int  slotOf (int fxIndex) const { for (int i = 0; i < kNumShown; ++i) if (disp[i] == fxIndex) return i; return -1; }
    bool canMoveUp   (int fxIndex) const { return slotOf (fxIndex) > 0; }
    bool canMoveDown (int fxIndex) const { const int s = slotOf (fxIndex); return s >= 0 && s < kNumShown - 1; }

private:
    // Read the processor's 5-slot order and project it to the 4 shown FX in display
    // order (EQ_ = 4 is dropped; it always runs last regardless of position).
    void syncFromProc()
    {
        int full[5]; proc.getFxOrder (full);
        int k = 0;
        for (int slot = 0; slot < 5; ++slot)
            if (full[slot] != FXChain::EQ_ && k < kNumShown) disp[k++] = full[slot];
        for (; k < kNumShown; ++k) disp[k] = k;   // defensive fill (never expected)
    }

    void timerCallback() override
    {
        int prev[kNumShown]; for (int i = 0; i < kNumShown; ++i) prev[i] = disp[i];
        syncFromProc();
        bool diff = false;
        for (int i = 0; i < kNumShown; ++i) diff = diff || (prev[i] != disp[i]);
        if (diff) { layoutBlocks(); repaint(); for (auto& b : blocks) if (b) b->repaint(); }
    }

    struct KnobDef  { const char* pid; const char* name; const char* help = nullptr; };
    struct BlockDef { const char* title; juce::Colour tint; const char* enablePid; std::vector<KnobDef> knobs;
                      const char* voicesPid = nullptr; };   // optional 1|2 selector (chorus)

    static const std::array<BlockDef, kNumShown>& defs()
    {
        namespace ID = ParamID;
        static const std::array<BlockDef, kNumShown> d { {
            { "CHORUS", juce::Colour (0xff46c9b0), ID::fxChorusOn,
              { { ID::chorusRate, "RATE" }, { ID::chorusDepth, "DEPTH" }, { ID::chorusMix, "MIX" } }, ID::chorusVoices },
            { "DELAY",  juce::Colour (0xff6ea8ff), ID::fxDelayOn,
              { { ID::delayTime, "TIME" }, { ID::delayFeedback, "FBK" }, { ID::delaySpread, "PNG" }, { ID::delayMix, "MIX" } } },
            { "REVERB", juce::Colour (0xffb07cff), ID::fxReverbOn,
              { { ID::reverbSize, "SIZE" }, { ID::reverbDamp, "DAMP" }, { ID::reverbWidth, "WIDTH" },
                { ID::reverbMix, "MIX" },
                { ID::reverbMotion, "MOTION", "slow tail modulation: smears the metallic ring so pads swim (0 = static)" } } },
            { "SAT + WIDTH", juce::Colour (0xfff0a04b), ID::fxWidthOn,
              { { ID::fxSat, "SAT", "tube saturation (velocity-sensitive): warm soft overdrive up to noon, hardening toward a fuzz above" },
                { ID::stereoWidth, "WIDTH" } } },
        } };
        return d;
    }

    // ---- one effect block ---------------------------------------------------
    struct FXBlock : juce::Component
    {
        FXBlock (FXPanel& ownerPanel, VASynthProcessor& p, int fxIndex, const BlockDef& def)
            : owner (ownerPanel), fx (fxIndex), title (def.title), tint (def.tint)
        {
            enableParam = dynamic_cast<juce::AudioParameterBool*> (p.apvts.getParameter (def.enablePid));
            // Repaint the bar whenever the enable changes (automation / preset load).
            enableAtt = std::make_unique<juce::ParameterAttachment> (
                *p.apvts.getParameter (def.enablePid), [this] (float) { repaint(); }, nullptr);
            for (auto& kd : def.knobs)
            {
                auto* k = new RotaryKnob (p.apvts, kd.pid, kd.name, p.getMidiLearn());
                if (kd.help) k->setHelp (kd.help);
                knobs.add (k);
                addAndMakeVisible (k);
            }
            if (def.voicesPid != nullptr)
            {
                voices = std::make_unique<HSelector> (p.apvts, def.voicesPid, p.getMidiLearn(), juce::StringArray { "1", "2" });
                voices->setHelp ("Chorus voices: 2 adds a second decorrelated tap (thicker, wider)");
                addAndMakeVisible (*voices);
            }
        }

        void paint (juce::Graphics& g) override
        {
            g.setColour (VASynthLookAndFeel::panel());
            g.fillRoundedRectangle (getLocalBounds().toFloat(), 5.0f);

            const bool on = enableParam != nullptr && enableParam->get();
            auto bar = barArea();
            g.setColour (on ? tint : VASynthLookAndFeel::track().darker (0.35f));
            g.fillRoundedRectangle (bar.toFloat(), 4.0f);
            if (on) { g.setColour (tint.brighter (0.5f)); g.drawRoundedRectangle (bar.toFloat().reduced (0.7f), 4.0f, 1.0f); }

            const auto fg = on ? chrome::onTint() : VASynthLookAndFeel::dim();
            g.setColour (fg);
            g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
            g.drawText (title, bar.withTrimmedLeft (8).withTrimmedRight (2 * kArrowW), juce::Justification::centredLeft, false);

            // Reorder chevrons at the right of the bar; dimmed at the ends of the chain (can't move).
            drawChevron (g, upArrowArea(),   true,  fg, owner.canMoveUp   (fx));
            drawChevron (g, downArrowArea(), false, fg, owner.canMoveDown (fx));

            if (voices)   // label above the 1|2 selector, matching the knob name style
            {
                g.setColour (VASynthLookAndFeel::dim());
                g.setFont (juce::Font (juce::FontOptions (9.5f)));
                g.drawText ("VOICES", voicesLabelArea.withHeight (13), juce::Justification::centred, false);
            }
        }

        static void drawChevron (juce::Graphics& g, juce::Rectangle<int> r, bool up, juce::Colour c, bool enabled)
        {
            auto a = r.toFloat().reduced (7.0f, 8.0f);
            juce::Path p;
            if (up) { p.startNewSubPath (a.getX(), a.getBottom()); p.lineTo (a.getCentreX(), a.getY());      p.lineTo (a.getRight(), a.getBottom()); }
            else    { p.startNewSubPath (a.getX(), a.getY());      p.lineTo (a.getCentreX(), a.getBottom()); p.lineTo (a.getRight(), a.getY()); }
            g.setColour (c.withMultipliedAlpha (enabled ? 1.0f : 0.25f));
            g.strokePath (p, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        void resized() override
        {
            auto body = getLocalBounds().withTrimmedTop (kBarH + 3).reduced (5, 2);
            const int n = knobs.size();
            if (n == 0) return;
            if (voices)   // reserve a narrow strip on the right for the 1|2 selector + its label
            {
                voicesLabelArea = body.removeFromRight (56);
                voices->setBounds (voicesLabelArea.reduced (5, 2).withTrimmedTop (13));
            }
            const int kw = body.getWidth() / n;
            for (int i = 0; i < n; ++i)
                knobs[i]->setBounds ((i < n - 1 ? body.removeFromLeft (kw) : body).reduced (3, 0));
        }

        // A click on the NAME BAR either reorders (chevrons) or toggles the effect. There is no drag
        // gesture, so a slip while grabbing a knob (the knobs sit BELOW the bar and take their own
        // mouse events) can never move a block. Ignore anything that travelled like a drag.
        void mouseUp (const juce::MouseEvent& e) override
        {
            if (e.getDistanceFromDragStart() >= 8) return;
            const auto pos = e.getPosition();
            if (! barArea().contains (pos)) return;                        // below the bar -> knobs' territory
            if (upArrowArea().contains (pos))   { owner.moveBlock (fx, -1); return; }
            if (downArrowArea().contains (pos)) { owner.moveBlock (fx, +1); return; }
            if (enableParam != nullptr)                                    // rest of the bar toggles on/off
            {
                enableParam->beginChangeGesture();
                enableParam->setValueNotifyingHost (enableParam->get() ? 0.0f : 1.0f);
                enableParam->endChangeGesture();
            }
        }

        juce::Rectangle<int> barArea()    const { return getLocalBounds().removeFromTop (kBarH); }
        juce::Rectangle<int> upArrowArea()   const { auto a = barArea().removeFromRight (2 * kArrowW); return a.removeFromLeft  (kArrowW); }
        juce::Rectangle<int> downArrowArea() const { auto a = barArea().removeFromRight (2 * kArrowW); return a.removeFromRight (kArrowW); }

        static constexpr int kArrowW = 22;

        FXPanel& owner;
        int  fx;
        juce::String title;
        juce::Colour tint;
        juce::AudioParameterBool* enableParam = nullptr;
        std::unique_ptr<juce::ParameterAttachment> enableAtt;
        juce::OwnedArray<RotaryKnob> knobs;
        std::unique_ptr<HSelector> voices;              // optional 1|2 selector (chorus)
        juce::Rectangle<int> voicesLabelArea;
    };

    void layoutBlocks()
    {
        const auto content = contentBounds();
        const int blockH = juce::jmax (1, (content.getHeight() - (kNumShown - 1) * kGap) / kNumShown);
        for (int slot = 0; slot < kNumShown; ++slot)
            blocks[(size_t) disp[slot]]->setBounds (content.getX(), content.getY() + slot * (blockH + kGap),
                                                    content.getWidth(), blockH);
    }

    juce::Rectangle<int> contentBounds() const { return chrome::sectionContent (getLocalBounds()); }

    static constexpr int kBarH = 26;
    static constexpr int kGap  = 5;

    VASynthProcessor& proc;
    juce::Colour tint { 0xff5ecb8a };
    std::array<std::unique_ptr<FXBlock>, kNumShown> blocks;
    int disp[kNumShown] { 3, 0, 1, 2 };   // the 4 FX in display (= processing) order; WIDTH first

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FXPanel)
};
