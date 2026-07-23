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
// BAR: the bar glows in the FX tint when the effect is on and darkens when off
// (tap it to toggle) — the on/off IS the label, no separate switch. Knobs are
// MIDI-learnable and refuse keyboard focus, so QWERTY note input keeps working.
//
// The chain runs in a FIXED order (WIDTH first, EQ always last) — the blocks are
// laid out in that processing order but do NOT reorder, so grabbing a knob never
// nudges a block. (The processor still keeps a 5-slot order[] so factory presets /
// old sessions load correctly; there is just no drag-to-reorder in the UI.)
//
// K1: the per-part EQ is NOT one of these blocks — it is a fixed final stage with
// its own dedicated section (EQPanel).
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
            blocks[(size_t) i] = std::make_unique<FXBlock> (proc, i, defs()[(size_t) i]);
            addAndMakeVisible (*blocks[(size_t) i]);
        }
        startTimerHz (8);      // resync the visual order if the chain order changes (e.g. preset load)
    }

    void paint (juce::Graphics& g) override
    { chrome::section (g, getLocalBounds(), "FX", tint); }

    void resized() override { layoutBlocks(); }

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
        if (diff) { layoutBlocks(); repaint(); }
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
        FXBlock (VASynthProcessor& p, int fxIndex, const BlockDef& def)
            : fx (fxIndex), title (def.title), tint (def.tint)
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

            g.setColour (on ? chrome::onTint() : VASynthLookAndFeel::dim());
            g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
            g.drawText (title, bar.withTrimmedLeft (8), juce::Justification::centredLeft, false);

            if (voices)   // label above the 1|2 selector, matching the knob name style
            {
                g.setColour (VASynthLookAndFeel::dim());
                g.setFont (juce::Font (juce::FontOptions (9.5f)));
                g.drawText ("VOICES", voicesLabelArea.withHeight (13), juce::Justification::centred, false);
            }
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

        // Tap the NAME BAR to toggle the effect on/off. (No drag-to-reorder — the blocks
        // are fixed, so a slip while grabbing a knob can't nudge a block.) Knobs (below the
        // bar) handle their own mouse events.
        void mouseUp (const juce::MouseEvent& e) override
        {
            if (e.getPosition().y < kBarH && e.getDistanceFromDragStart() < 8 && enableParam != nullptr)
            {
                enableParam->beginChangeGesture();
                enableParam->setValueNotifyingHost (enableParam->get() ? 0.0f : 1.0f);
                enableParam->endChangeGesture();
            }
        }

        juce::Rectangle<int> barArea() const { return getLocalBounds().removeFromTop (kBarH); }

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
