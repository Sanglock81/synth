#pragma once
#include "VASynthLookAndFeel.h"
#include "Widgets.h"
#include "../PluginProcessor.h"
#include <array>
#include <vector>

// ============================================================================
// FX panel — the far-right column. Four effect blocks (chorus / delay / reverb /
// width), each a rotary-knob strip with an on/off kill toggle, stacked in the
// current chain order. Drag a block by its header (finger or mouse) to reorder;
// on drop the new order is committed to the processor, which crossfades the audio
// chain click-free. Knobs are MIDI-learnable and refuse keyboard focus like every
// other control, so QWERTY note input keeps working.
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
    {
        auto r = getLocalBounds().toFloat().reduced (2.0f);
        g.setColour (VASynthLookAndFeel::panelLight().interpolatedWith (tint, 0.10f));
        g.fillRoundedRectangle (r, 7.0f);
        g.setColour (tint.withAlpha (0.55f));
        g.drawRoundedRectangle (r, 7.0f, 1.2f);

        g.setColour (VASynthLookAndFeel::ink());
        g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
        g.drawText ("FX  (drag to reorder)", getLocalBounds().removeFromTop (kHeaderH).withTrimmedLeft (10),
                    juce::Justification::centredLeft, false);
    }

    void resized() override { layoutBlocks(); }

private:
    // Keep the displayed order in step with the processor when it changes from
    // outside the panel (preset/state load). Never fights an in-progress drag.
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
            { "Chorus", juce::Colour (0xff46c9b0), ID::fxChorusOn,
              { { ID::chorusRate, "Rate" }, { ID::chorusDepth, "Depth" }, { ID::chorusMix, "Mix" } } },
            { "Delay",  juce::Colour (0xff6ea8ff), ID::fxDelayOn,
              { { ID::delayTime, "Time" }, { ID::delayFeedback, "Fbk" }, { ID::delayMix, "Mix" }, { ID::delaySpread, "Png" } } },
            { "Reverb", juce::Colour (0xffb07cff), ID::fxReverbOn,
              { { ID::reverbSize, "Size" }, { ID::reverbDamp, "Damp" }, { ID::reverbWidth, "Wid" }, { ID::reverbMix, "Mix" } } },
            { "Width",  juce::Colour (0xfff0a04b), ID::fxWidthOn,
              { { ID::stereoWidth, "Width" } } },
        } };
        return d;
    }

    // ---- one effect block ---------------------------------------------------
    struct FXBlock : juce::Component
    {
        FXBlock (VASynthProcessor& p, int fxIndex, const BlockDef& def, FXPanel& ownerPanel)
            : fx (fxIndex), owner (ownerPanel), title (def.title), tint (def.tint)
        {
            enable = std::make_unique<PowerToggle> (p.apvts, def.enablePid, "on");
            addAndMakeVisible (*enable);
            for (auto& kd : def.knobs)
            {
                auto* k = new RotaryKnob (p.apvts, kd.pid, kd.name, p.getMidiLearn());
                knobs.add (k);
                addAndMakeVisible (k);
            }
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat().reduced (2.0f);
            g.setColour (VASynthLookAndFeel::panel().interpolatedWith (tint, 0.14f));
            g.fillRoundedRectangle (r, 5.0f);
            g.setColour (tint.withAlpha (dragging ? 0.95f : 0.5f));
            g.drawRoundedRectangle (r, 5.0f, dragging ? 2.0f : 1.0f);

            g.setColour (VASynthLookAndFeel::ink());
            g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
            g.drawText (title, headerArea().withTrimmedLeft (46), juce::Justification::centredLeft, false);

            // grip dots (top-right) hint the header is a drag handle
            g.setColour (VASynthLookAndFeel::dim().withAlpha (0.6f));
            auto grip = headerArea().removeFromRight (18);
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 2; ++col)
                    g.fillEllipse ((float) grip.getX() + 3.0f + (float) col * 6.0f,
                                   (float) grip.getCentreY() - 6.0f + (float) row * 6.0f, 2.2f, 2.2f);
        }

        void resized() override
        {
            auto hb = headerArea();
            enable->setBounds (hb.removeFromLeft (42).reduced (5, 6));
            auto body = getLocalBounds().withTrimmedTop (kHeaderH).reduced (5, 2);
            juce::FlexBox fb; fb.flexDirection = juce::FlexBox::Direction::row;
            for (auto* k : knobs) fb.items.add (juce::FlexItem (*k).withFlex (1.0f).withMargin (2.0f));
            fb.performLayout (body);
        }

        // Drag from the header only (so knob/toggle interaction is untouched).
        void mouseDown (const juce::MouseEvent& e) override
        {
            dragArmed = e.getPosition().y < kHeaderH;
            if (dragArmed) owner.beginDrag (fx, owner.getLocalPoint (this, e.position).y);
        }
        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (dragArmed) owner.dragTo (fx, owner.getLocalPoint (this, e.position).y);
        }
        void mouseUp (const juce::MouseEvent&) override
        {
            if (dragArmed) owner.endDrag();
            dragArmed = false;
        }

        juce::Rectangle<int> headerArea() const { return getLocalBounds().removeFromTop (kHeaderH); }

        int  fx;
        FXPanel& owner;
        juce::String title;
        juce::Colour tint;
        bool dragging  = false;
        bool dragArmed = false;
        std::unique_ptr<PowerToggle> enable;
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
        const int blockH = juce::jmax (1, content.getHeight() / 4);
        for (int slot = 0; slot < 4; ++slot)
        {
            const int fx = order[slot];
            if (fx == draggingFx) continue;
            blocks[(size_t) fx]->setBounds (content.getX(), content.getY() + slot * blockH,
                                            content.getWidth(), blockH - 2);
        }
        if (draggingFx >= 0)
        {
            const int y = juce::jlimit (content.getY(), content.getBottom() - blockH,
                                        (int) dragY - blockH / 2);
            blocks[(size_t) draggingFx]->setBounds (content.getX(), y, content.getWidth(), blockH - 2);
            blocks[(size_t) draggingFx]->toFront (false);
        }
    }

    juce::Rectangle<int> contentBounds() const { return getLocalBounds().withTrimmedTop (kHeaderH).reduced (6, 4); }

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

    static constexpr int kHeaderH = 26;

    VASynthProcessor& proc;
    juce::Colour tint { 0xff8a929c };
    std::array<std::unique_ptr<FXBlock>, 4> blocks;
    int order[4] { 0, 1, 2, 3 };
    int draggingFx = -1;
    float dragY = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FXPanel)
};
