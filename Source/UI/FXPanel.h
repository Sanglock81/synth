#pragma once
#include "VASynthLookAndFeel.h"
#include "PanelChrome.h"
#include "Widgets.h"
#include "../PluginProcessor.h"
#include <array>
#include <vector>

// ============================================================================
// FX panel — a centre section (signal-flow order). Four effect blocks (chorus /
// delay / reverb / width), each a knob strip beneath a backlit NAME BAR: the bar
// glows in the FX tint when the effect is on and darkens when off (tap it to
// toggle) — the on/off IS the label, no separate switch. Drag a block by its bar
// (finger or mouse) to reorder; on drop the new chain order is committed and the
// audio chain crossfades click-free. Knobs are MIDI-learnable and refuse keyboard
// focus, so QWERTY note input keeps working.
// ============================================================================

class FXPanel : public juce::Component,
                private juce::Timer
{
public:
    explicit FXPanel (VASynthProcessor& p) : proc (p)
    {
        proc.getFxOrder (order);

        for (int fx = 0; fx < 4; ++fx)
        {
            blocks[(size_t) fx] = std::make_unique<FXBlock> (proc, fx, defs()[(size_t) fx], *this);
            addAndMakeVisible (*blocks[(size_t) fx]);
        }
        startTimerHz (15);      // resync the visual order if it changes elsewhere (e.g. preset load)
    }

    void paint (juce::Graphics& g) override
    { chrome::section (g, getLocalBounds(), "FX  -  drag to reorder", tint); }

    void resized() override { layoutBlocks(); }

private:
    void timerCallback() override
    {
        if (draggingFx >= 0) return;
        int cur[4]; proc.getFxOrder (cur);
        bool diff = false;
        for (int i = 0; i < 4; ++i) diff = diff || (cur[i] != order[i]);
        if (diff) { for (int i = 0; i < 4; ++i) order[i] = cur[i]; layoutBlocks(); repaint(); }
    }

    struct KnobDef  { const char* pid; const char* name; };
    struct BlockDef { const char* title; juce::Colour tint; const char* enablePid; std::vector<KnobDef> knobs; };

    static const std::array<BlockDef, 4>& defs()
    {
        namespace ID = ParamID;
        static const std::array<BlockDef, 4> d { {
            { "CHORUS", juce::Colour (0xff46c9b0), ID::fxChorusOn,
              { { ID::chorusRate, "RATE" }, { ID::chorusDepth, "DEPTH" }, { ID::chorusMix, "MIX" } } },
            { "DELAY",  juce::Colour (0xff6ea8ff), ID::fxDelayOn,
              { { ID::delayTime, "TIME" }, { ID::delayFeedback, "FBK" }, { ID::delaySpread, "PNG" }, { ID::delayMix, "MIX" } } },
            { "REVERB", juce::Colour (0xffb07cff), ID::fxReverbOn,
              { { ID::reverbSize, "SIZE" }, { ID::reverbDamp, "DAMP" }, { ID::reverbWidth, "WIDTH" }, { ID::reverbMix, "MIX" } } },
            { "WIDTH",  juce::Colour (0xfff0a04b), ID::fxWidthOn,
              { { ID::stereoWidth, "WIDTH" } } },
        } };
        return d;
    }

    // ---- one effect block ---------------------------------------------------
    struct FXBlock : juce::Component
    {
        FXBlock (VASynthProcessor& p, int fxIndex, const BlockDef& def, FXPanel& ownerPanel)
            : fx (fxIndex), owner (ownerPanel), title (def.title), tint (def.tint)
        {
            enableParam = dynamic_cast<juce::AudioParameterBool*> (p.apvts.getParameter (def.enablePid));
            // Repaint the bar whenever the enable changes (automation / preset load).
            enableAtt = std::make_unique<juce::ParameterAttachment> (
                *p.apvts.getParameter (def.enablePid), [this] (float) { repaint(); }, nullptr);
            for (auto& kd : def.knobs)
            {
                auto* k = new RotaryKnob (p.apvts, kd.pid, kd.name, p.getMidiLearn());
                knobs.add (k);
                addAndMakeVisible (k);
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

            // grip dots (left) hint the bar is a drag handle
            auto grip = bar.removeFromLeft (18);
            g.setColour (on ? chrome::onTint().withAlpha (0.6f) : VASynthLookAndFeel::dim());
            for (int col = 0; col < 2; ++col)
                for (int row = 0; row < 3; ++row)
                    g.fillEllipse ((float) grip.getCentreX() - 3.0f + (float) col * 4.0f,
                                   (float) grip.getCentreY() - 6.0f + (float) row * 5.0f, 2.4f, 2.4f);

            g.setColour (on ? chrome::onTint() : VASynthLookAndFeel::dim());
            g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
            g.drawText (title, bar.withTrimmedLeft (2), juce::Justification::centredLeft, false);

            if (dragging) { g.setColour (tint.withAlpha (0.9f)); g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 5.0f, 2.0f); }
        }

        void resized() override
        {
            auto body = getLocalBounds().withTrimmedTop (kBarH + 3).reduced (5, 2);
            const int n = knobs.size();
            if (n == 0) return;
            const int kw = body.getWidth() / n;
            for (int i = 0; i < n; ++i)
                knobs[i]->setBounds ((i < n - 1 ? body.removeFromLeft (kw) : body).reduced (3, 0));
        }

        // Tap the bar toggles on/off; drag the bar reorders. Knobs (below) untouched.
        void mouseDown (const juce::MouseEvent& e) override
        {
            dragArmed = e.getPosition().y < kBarH;
            movedFar  = false;
            if (dragArmed) owner.beginDrag (fx, owner.getLocalPoint (this, e.position).y);
        }
        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (! dragArmed) return;
            if (e.getDistanceFromDragStart() > 8) movedFar = true;
            owner.dragTo (fx, owner.getLocalPoint (this, e.position).y);
        }
        void mouseUp (const juce::MouseEvent&) override
        {
            if (dragArmed)
            {
                owner.endDrag();
                if (! movedFar && enableParam != nullptr)     // a tap -> toggle on/off
                {
                    enableParam->beginChangeGesture();
                    enableParam->setValueNotifyingHost (enableParam->get() ? 0.0f : 1.0f);
                    enableParam->endChangeGesture();
                }
            }
            dragArmed = false;
        }

        juce::Rectangle<int> barArea() const { return getLocalBounds().removeFromTop (kBarH); }

        int  fx;
        FXPanel& owner;
        juce::String title;
        juce::Colour tint;
        bool dragging  = false;
        bool dragArmed = false;
        bool movedFar  = false;
        juce::AudioParameterBool* enableParam = nullptr;
        std::unique_ptr<juce::ParameterAttachment> enableAtt;
        juce::OwnedArray<RotaryKnob> knobs;
    };

    // ---- drag-reorder -------------------------------------------------------
    void beginDrag (int fx, float panelY)
    {
        draggingFx = fx;
        dragY = panelY;
        blocks[(size_t) fx]->dragging = true;
        blocks[(size_t) fx]->toFront (false);
        layoutBlocks();
    }

    void dragTo (int fx, float panelY)
    {
        if (draggingFx != fx) return;
        dragY = panelY;
        const auto content = contentBounds();
        const int blockH = juce::jmax (1, content.getHeight() / 4);
        const int targetSlot = juce::jlimit (0, 3, (int) ((panelY - content.getY()) / blockH));
        const int curSlot = slotOf (fx);
        if (targetSlot != curSlot) moveInOrder (curSlot, targetSlot);
        layoutBlocks();
    }

    void endDrag()
    {
        if (draggingFx < 0) return;
        blocks[(size_t) draggingFx]->dragging = false;
        draggingFx = -1;
        proc.setFxOrder (order);      // commit -> audio chain crossfades to the new order
        layoutBlocks();
        repaint();
    }

    void layoutBlocks()
    {
        const auto content = contentBounds();
        const int blockH = juce::jmax (1, (content.getHeight() - 3 * kGap) / 4);
        for (int slot = 0; slot < 4; ++slot)
        {
            const int fx = order[slot];
            if (fx == draggingFx) continue;
            blocks[(size_t) fx]->setBounds (content.getX(), content.getY() + slot * (blockH + kGap),
                                            content.getWidth(), blockH);
        }
        if (draggingFx >= 0)
        {
            const int y = juce::jlimit (content.getY(), content.getBottom() - blockH,
                                        (int) dragY - blockH / 2);
            blocks[(size_t) draggingFx]->setBounds (content.getX(), y, content.getWidth(), blockH);
            blocks[(size_t) draggingFx]->toFront (false);
        }
    }

    juce::Rectangle<int> contentBounds() const { return chrome::sectionContent (getLocalBounds()); }

    int slotOf (int fx) const
    {
        for (int i = 0; i < 4; ++i) if (order[i] == fx) return i;
        return 0;
    }
    void moveInOrder (int from, int to)
    {
        const int fx = order[from];
        if (from < to) for (int i = from; i < to; ++i) order[i] = order[i + 1];
        else           for (int i = from; i > to; --i) order[i] = order[i - 1];
        order[to] = fx;
    }

    static constexpr int kBarH = 26;
    static constexpr int kGap  = 5;

    VASynthProcessor& proc;
    juce::Colour tint { 0xff5ecb8a };
    std::array<std::unique_ptr<FXBlock>, 4> blocks;
    int order[4] { 0, 1, 2, 3 };
    int draggingFx = -1;
    float dragY = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FXPanel)
};
