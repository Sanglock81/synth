#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "VASynthLookAndFeel.h"
#include "PanelChrome.h"
#include "Widgets.h"
#include "../PluginProcessor.h"
#include "../DSP/ParametricEQ.h"

// ============================================================================
// Master parametric-EQ panel (bottom of the right column): a backlit ON name-bar,
// a live response curve computed from the band parameters, and four bands of knobs
// (low shelf / two bells / high shelf). The curve uses its OWN ParametricEQ fed the
// current param values — it never touches the audio-thread EQ. Focus-refusing;
// knobs are MIDI-learnable.
// ============================================================================

class EQPanel : public juce::Component,
                private juce::Timer
{
public:
    explicit EQPanel (VASynthProcessor& p) : proc (p)
    {
        setWantsKeyboardFocus (false);
        curveEq.prepare (48000.0);

        eqOn = dynamic_cast<juce::AudioParameterBool*> (proc.apvts.getParameter (ParamID::eqOn));
        eqOnAtt = std::make_unique<juce::ParameterAttachment> (*proc.apvts.getParameter (ParamID::eqOn), [this] (float) { repaint(); }, nullptr);

        struct KD { const char* pid; const char* name; int col; };
        const KD kd[] {
            { ParamID::eqLsFreq, "FREQ", 0 }, { ParamID::eqLsGain, "LOW",  0 },
            { ParamID::eqLmFreq, "FREQ", 1 }, { ParamID::eqLmGain, "L-MID", 1 }, { ParamID::eqLmQ, "Q", 1 },
            { ParamID::eqHmFreq, "FREQ", 2 }, { ParamID::eqHmGain, "H-MID", 2 }, { ParamID::eqHmQ, "Q", 2 },
            { ParamID::eqHsFreq, "FREQ", 3 }, { ParamID::eqHsGain, "HIGH", 3 } };
        for (auto& d : kd)
        {
            auto* k = new RotaryKnob (proc.apvts, d.pid, d.name, proc.getMidiLearn());
            knobs.add (k); cols.add (d.col); addAndMakeVisible (k);
        }
        startTimerHz (20);   // refresh the curve as params move
    }

    void paint (juce::Graphics& g) override
    {
        const auto tViz = juce::Colour (0xff67c0c8);
        auto content = chrome::section (g, getLocalBounds(), "EQ  -  master", tViz);

        // ON name-bar (backlit): tap toggles.
        const bool on = eqOn != nullptr && eqOn->get();
        barR = content.removeFromTop (22); content.removeFromTop (4);
        g.setColour (on ? tViz : VASynthLookAndFeel::track().darker (0.35f));
        g.fillRoundedRectangle (barR.toFloat(), 4.0f);
        g.setColour (on ? chrome::onTint() : VASynthLookAndFeel::dim());
        g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        g.drawText (on ? "  ON" : "  OFF (tap)", barR, juce::Justification::centredLeft, false);

        // response curve
        auto cv = content.removeFromTop (juce::jmax (40, content.getHeight() * 4 / 10));
        curveArea = cv;
        g.setColour (VASynthLookAndFeel::track());
        g.fillRoundedRectangle (cv.toFloat(), 4.0f);
        g.setColour (VASynthLookAndFeel::dim().withAlpha (0.35f));
        g.drawLine ((float) cv.getX(), (float) cv.getCentreY(), (float) cv.getRight(), (float) cv.getCentreY(), 1.0f);   // 0 dB

        curveEq.setBands (band (ParamID::eqLsFreq, ParamID::eqLsGain, nullptr),
                          band (ParamID::eqLmFreq, ParamID::eqLmGain, ParamID::eqLmQ),
                          band (ParamID::eqHmFreq, ParamID::eqHmGain, ParamID::eqHmQ),
                          band (ParamID::eqHsFreq, ParamID::eqHsGain, nullptr));
        juce::Path curve;
        const int w = juce::jmax (2, cv.getWidth());
        for (int x = 0; x < w; ++x)
        {
            const double t = (double) x / (w - 1);
            const double freq = 20.0 * std::pow (1000.0, t);                 // 20 Hz .. 20 kHz log
            const float db = on ? curveEq.magnitudeDb (freq) : 0.0f;
            const float y = cv.getCentreY() - juce::jlimit (-18.0f, 18.0f, db) / 18.0f * (cv.getHeight() * 0.46f);
            if (x == 0) curve.startNewSubPath ((float) cv.getX(), y);
            else        curve.lineTo ((float) (cv.getX() + x), y);
        }
        g.setColour (on ? tViz : VASynthLookAndFeel::dim());
        g.strokePath (curve, juce::PathStrokeType (2.0f));
    }

    void resized() override
    {
        auto content = chrome::sectionContent (getLocalBounds());
        content.removeFromTop (22 + 4);                                     // ON bar
        content.removeFromTop (juce::jmax (40, content.getHeight() * 4 / 10) + 5);   // curve (approx; paint owns exact)
        // 4 band columns
        const int n = 4, gap = 4;
        const int cw = juce::jmax (10, (content.getWidth() - (n - 1) * gap) / n);
        std::array<juce::Rectangle<int>, 4> colR;
        for (int c = 0; c < n; ++c) { colR[(size_t) c] = content.removeFromLeft (cw); content.removeFromLeft (gap); }
        std::array<int, 4> placed { 0, 0, 0, 0 };
        std::array<int, 4> count  { 0, 0, 0, 0 };
        for (int i = 0; i < knobs.size(); ++i) ++count[(size_t) cols[i]];
        for (int i = 0; i < knobs.size(); ++i)
        {
            const int c = cols[i];
            const int kh = juce::jmax (18, colR[(size_t) c].getHeight() / count[(size_t) c]);
            knobs[i]->setBounds (colR[(size_t) c].getX(), colR[(size_t) c].getY() + placed[(size_t) c] * kh, colR[(size_t) c].getWidth(), kh);
            ++placed[(size_t) c];
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.getDistanceFromDragStart() > 8) return;
        if (barR.contains (e.getPosition()) && eqOn != nullptr)
        {
            eqOn->beginChangeGesture();
            eqOn->setValueNotifyingHost (eqOn->get() ? 0.0f : 1.0f);
            eqOn->endChangeGesture();
        }
    }

private:
    ParametricEQ::Band band (const char* fId, const char* gId, const char* qId) const
    {
        return { proc.apvts.getRawParameterValue (fId)->load(),
                 proc.apvts.getRawParameterValue (gId)->load(),
                 qId != nullptr ? proc.apvts.getRawParameterValue (qId)->load() : 0.7f };
    }
    void timerCallback() override { repaint (curveArea); }

    VASynthProcessor& proc;
    ParametricEQ curveEq;
    juce::AudioParameterBool* eqOn = nullptr;
    std::unique_ptr<juce::ParameterAttachment> eqOnAtt;
    juce::OwnedArray<RotaryKnob> knobs;
    juce::Array<int> cols;
    juce::Rectangle<int> barR, curveArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EQPanel)
};
