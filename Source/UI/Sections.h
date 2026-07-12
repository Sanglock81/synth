#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "VASynthLookAndFeel.h"
#include "PanelChrome.h"
#include "Widgets.h"
#include "../PluginProcessor.h"

// ============================================================================
// R2 centre synth sections, in signal-flow order. Each owns real APVTS-bound
// widgets (knobs/faders/selectors), paints the signed-off filled-tint chrome +
// inset sub-boxes, and lays its controls out to the mockup geometry. All widgets
// refuse keyboard focus (QWERTY note input keeps working) and are MIDI-learnable.
//
// Reconciliation with the real parameter set (mockup showed a few not-yet-built
// R3 controls): oscillator FINE -> per-osc LEVEL; a small ON kill toggle is
// surfaced (osc3 ships off); filter DRIVE (R3) dropped, VEL>CUT shown instead;
// the wavetable (WT) wave arrives in R3.
// ============================================================================

namespace sectiontint
{
    inline juce::Colour osc()  { return VASynthLookAndFeel::accent(); }
    inline juce::Colour filt() { return juce::Colour (0xff6ea8ff); }
    inline juce::Colour env()  { return juce::Colour (0xffb07cff); }
    inline juce::Colour lfo()  { return juce::Colour (0xfff0a04b); }
    inline juce::Colour fx()   { return juce::Colour (0xff5ecb8a); }
}

// ---------------------------------------------------------------------------
// OSCILLATORS: one sub-box per osc — [ON | wave selector] across the top, then
// OCTAVE / DETUNE / PW / LEVEL knobs.
class OscSection : public juce::Component
{
public:
    explicit OscSection (VASynthProcessor& p)
    {
        namespace ID = ParamID;
        const char* onIds[]   { ID::osc1On, ID::osc2On, ID::osc3On };
        const char* waveIds[] { ID::osc1Wave, ID::osc2Wave, ID::osc3Wave };
        const char* octIds[]  { ID::osc1Octave, ID::osc2Octave, ID::osc3Octave };
        const char* detIds[]  { ID::osc1Detune, ID::osc2Detune, ID::osc3Detune };
        const char* pwIds[]   { ID::osc1PW, ID::osc2PW, ID::osc3PW };
        const char* lvlIds[]  { ID::osc1Level, ID::osc2Level, ID::osc3Level };
        const juce::StringArray waveLabels { "SAW", "SQR", "TRI", "SIN" };

        for (int i = 0; i < 3; ++i)
        {
            auto& o = oscs[(size_t) i];
            o.on   = std::make_unique<PowerToggle> (p.apvts, onIds[i], "ON");
            o.wave = std::make_unique<HSelector> (p.apvts, waveIds[i], p.getMidiLearn(), waveLabels);
            o.k[0] = std::make_unique<RotaryKnob> (p.apvts, octIds[i], "OCTAVE", p.getMidiLearn());
            o.k[1] = std::make_unique<RotaryKnob> (p.apvts, detIds[i], "DETUNE", p.getMidiLearn());
            o.k[2] = std::make_unique<RotaryKnob> (p.apvts, pwIds[i],  "PW",     p.getMidiLearn());
            o.k[3] = std::make_unique<RotaryKnob> (p.apvts, lvlIds[i], "LEVEL",  p.getMidiLearn());
            // LFO->PW: the pw mod is in pw units (0..1 linear), so it maps straight to the
            // knob's normalized offset. Shown on every oscillator's PW knob.
            o.k[2]->setModSource ([&p]() -> float { return p.lfoModForDest (3); });
            addAndMakeVisible (*o.on);   addAndMakeVisible (*o.wave);
            for (auto& k : o.k) addAndMakeVisible (*k);
        }
    }

    void paint (juce::Graphics& g) override
    {
        chrome::section (g, getLocalBounds(), "Oscillators", sectiontint::osc());
        for (auto& b : boxRects()) chrome::subBox (g, b, sectiontint::osc());
    }

    void resized() override
    {
        auto boxes = boxRects();
        for (int i = 0; i < 3; ++i)
        {
            auto c = chrome::subBoxContent (boxes[(size_t) i]);
            auto& o = oscs[(size_t) i];
            auto top = c.removeFromTop (26); c.removeFromTop (4);
            o.on->setBounds (top.removeFromLeft (36)); top.removeFromLeft (5);
            o.wave->setBounds (top);
            const int kw = c.getWidth() / 4;
            for (int k = 0; k < 4; ++k)
                o.k[(size_t) k]->setBounds ((k < 3 ? c.removeFromLeft (kw) : c).reduced (2, 0));
        }
    }

private:
    std::array<juce::Rectangle<int>, 3> boxRects() const
    {
        auto s = chrome::sectionContent (getLocalBounds());
        const int gap = 5, bh = juce::jmax (10, (s.getHeight() - 2 * gap) / 3);
        std::array<juce::Rectangle<int>, 3> r;
        for (int i = 0; i < 3; ++i) { r[(size_t) i] = s.removeFromTop (bh); s.removeFromTop (gap); }
        return r;
    }

    struct Osc
    {
        std::unique_ptr<PowerToggle> on;
        std::unique_ptr<HSelector> wave;
        std::array<std::unique_ptr<RotaryKnob>, 4> k;
    };
    std::array<Osc, 3> oscs;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OscSection)
};

// ---------------------------------------------------------------------------
// FILTER: type selector (tall) across the top, then a vertical column of knobs
// (knob-left, label-beside).
class FilterSection : public juce::Component
{
public:
    explicit FilterSection (VASynthProcessor& p)
    {
        namespace ID = ParamID;
        type = std::make_unique<HSelector> (p.apvts, ID::filterType, p.getMidiLearn(),
                                            juce::StringArray { "LP", "HP", "BP", "NOTCH" });
        addAndMakeVisible (*type);

        struct KD { const char* pid; const char* name; };
        const KD kd[] {
            { ID::filterCutoff,   "CUTOFF"  }, { ID::filterReso, "RESO" },
            { ID::filterEnvAmt,   "ENV AMT" }, { ID::filterKeytrack, "KEYTRK" },
            { ID::velToCutoff,    "VEL>CUT" } };
        for (auto& d : kd)
        {
            auto* k = new RotaryKnob (p.apvts, d.pid, d.name, p.getMidiLearn(), /*sideLabel*/ true);
            knobs.add (k); addAndMakeVisible (k);
            if (juce::String (d.pid) == ID::filterCutoff)
            {
                auto* cut = p.apvts.getParameter (ID::filterCutoff);
                k->setModSource ([&p, cut]() -> float          // LFO->cutoff: octaves -> normalized offset
                {
                    const float oct = p.lfoModForDest (2);
                    if (std::abs (oct) < 1.0e-5f) return 0.0f;
                    const auto& r = cut->getNormalisableRange();
                    const float base01 = cut->getValue();
                    const float modHz  = r.convertFrom0to1 (base01) * std::pow (2.0f, oct);
                    return r.convertTo0to1 (modHz) - base01;
                });
            }
        }
    }

    void paint (juce::Graphics& g) override
    { chrome::section (g, getLocalBounds(), "Filter", sectiontint::filt()); }

    void resized() override
    {
        auto s = chrome::sectionContent (getLocalBounds());
        type->setBounds (s.removeFromTop (juce::jmin (50, s.getHeight() / 3)));
        s.removeFromTop (6);
        const int n = knobs.size(), gap = 4;
        const int kh = juce::jmax (18, (s.getHeight() - (n - 1) * gap) / n);
        for (int i = 0; i < n; ++i) { knobs[i]->setBounds (s.removeFromTop (kh)); s.removeFromTop (gap); }
    }

private:
    std::unique_ptr<HSelector> type;
    juce::OwnedArray<RotaryKnob> knobs;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FilterSection)
};

// ---------------------------------------------------------------------------
// ENVELOPE: an AMP / MOD view toggle swaps the four ADSR faders between the amp
// and mod (filter) envelopes; E>PCH (mod-env->pitch) + VEL (vel->amp) knobs sit
// on the right, always visible.
class EnvSection : public juce::Component
{
public:
    explicit EnvSection (VASynthProcessor& p)
    {
        namespace ID = ParamID;
        const char* ampIds[] { ID::ampAttack, ID::ampDecay, ID::ampSustain, ID::ampRelease };
        const char* modIds[] { ID::fltAttack, ID::fltDecay, ID::fltSustain, ID::fltRelease };
        const char* names[]  { "A", "D", "S", "R" };
        for (int i = 0; i < 4; ++i)
        {
            amp.add (new LabelledFader (p.apvts, ampIds[i], names[i], p.getMidiLearn()));
            mod.add (new LabelledFader (p.apvts, modIds[i], names[i], p.getMidiLearn()));
            addAndMakeVisible (amp[i]); addChildComponent (mod[i]);
        }
        pitch = std::make_unique<RotaryKnob> (p.apvts, ID::fltEnvToPitch, "E>PCH", p.getMidiLearn());
        vel   = std::make_unique<RotaryKnob> (p.apvts, ID::velToAmp,      "VEL",   p.getMidiLearn());
        addAndMakeVisible (*pitch); addAndMakeVisible (*vel);

        for (int m = 0; m < 2; ++m)
        {
            auto* b = viewBtn.add (new juce::TextButton (m == 0 ? "AMP" : "MOD"));
            b->setClickingTogglesState (false);
            b->setWantsKeyboardFocus (false);
            b->setColour (juce::TextButton::buttonColourId, VASynthLookAndFeel::track());
            b->setColour (juce::TextButton::buttonOnColourId, VASynthLookAndFeel::accent());
            b->setColour (juce::TextButton::textColourOffId, VASynthLookAndFeel::ink());
            b->setColour (juce::TextButton::textColourOnId, juce::Colours::black);
            const int mm = m;
            b->onClick = [this, mm] { setMode (mm); };
            addAndMakeVisible (b);
        }
        setMode (0);
    }

    void paint (juce::Graphics& g) override
    { chrome::section (g, getLocalBounds(), "Envelope", sectiontint::env()); }

    void resized() override
    {
        auto s = chrome::sectionContent (getLocalBounds());
        auto sel = s.removeFromTop (30); s.removeFromTop (5);
        viewBtn[0]->setBounds (sel.removeFromLeft (sel.getWidth() / 2).reduced (2));
        viewBtn[1]->setBounds (sel.reduced (2));

        auto kk = s.removeFromRight (juce::jmax (48, s.getWidth() * 2 / 6));
        pitch->setBounds (kk.removeFromTop (kk.getHeight() / 2).reduced (2));
        vel->setBounds (kk.reduced (2));

        const int fw = s.getWidth() / 4;
        for (int i = 0; i < 4; ++i)
        {
            auto cell = (i < 3 ? s.removeFromLeft (fw) : s).reduced (2, 0);
            amp[i]->setBounds (cell); mod[i]->setBounds (cell);
        }
    }

private:
    void setMode (int m)
    {
        mode = m;
        for (int i = 0; i < 4; ++i) { amp[i]->setVisible (m == 0); mod[i]->setVisible (m == 1); }
        viewBtn[0]->setToggleState (m == 0, juce::dontSendNotification);
        viewBtn[1]->setToggleState (m == 1, juce::dontSendNotification);
    }

    juce::OwnedArray<LabelledFader> amp, mod;
    std::unique_ptr<RotaryKnob> pitch, vel;
    juce::OwnedArray<juce::TextButton> viewBtn;
    int mode = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EnvSection)
};

// ---------------------------------------------------------------------------
// LFO: one sub-box per LFO — DEST selector across the top, RATE/DEPTH knobs, and a
// vertical stack of waveform shape icons on the right.
class LfoSection : public juce::Component
{
public:
    explicit LfoSection (VASynthProcessor& p)
    {
        namespace ID = ParamID;
        const char* destIds[]  { ID::lfoDest,  ID::lfo2Dest,  ID::lfo3Dest  };
        const char* rateIds[]  { ID::lfoRate,  ID::lfo2Rate,  ID::lfo3Rate  };
        const char* depthIds[] { ID::lfoDepth, ID::lfo2Depth, ID::lfo3Depth };
        const char* shapeIds[] { ID::lfoShape, ID::lfo2Shape, ID::lfo3Shape };
        const juce::StringArray destLabels { "OFF", "PITCH", "CUTOFF", "PW" };

        for (int i = 0; i < 3; ++i)
        {
            auto& l = lfos[(size_t) i];
            l.dest  = std::make_unique<HSelector> (p.apvts, destIds[i], p.getMidiLearn(), destLabels);
            l.rate  = std::make_unique<RotaryKnob> (p.apvts, rateIds[i],  "RATE",  p.getMidiLearn());
            l.depth = std::make_unique<RotaryKnob> (p.apvts, depthIds[i], "DEPTH", p.getMidiLearn());
            l.shape = std::make_unique<ShapeSelector> (p.apvts, shapeIds[i], p.getMidiLearn());
            addAndMakeVisible (*l.dest);  addAndMakeVisible (*l.rate);
            addAndMakeVisible (*l.depth); addAndMakeVisible (*l.shape);
        }
    }

    void paint (juce::Graphics& g) override
    {
        chrome::section (g, getLocalBounds(), "LFO", sectiontint::lfo());
        for (auto& b : boxRects()) chrome::subBox (g, b, sectiontint::lfo());
    }

    void resized() override
    {
        auto boxes = boxRects();
        for (int i = 0; i < 3; ++i)
        {
            auto c = chrome::subBoxContent (boxes[(size_t) i]);
            auto& l = lfos[(size_t) i];
            l.dest->setBounds (c.removeFromTop (26)); c.removeFromTop (4);
            l.shape->setBounds (c.removeFromRight (40)); c.removeFromRight (5);
            l.rate->setBounds (c.removeFromLeft (c.getWidth() / 2).reduced (2, 0));
            l.depth->setBounds (c.reduced (2, 0));
        }
    }

private:
    std::array<juce::Rectangle<int>, 3> boxRects() const
    {
        auto s = chrome::sectionContent (getLocalBounds());
        const int gap = 5, bh = juce::jmax (10, (s.getHeight() - 2 * gap) / 3);
        std::array<juce::Rectangle<int>, 3> r;
        for (int i = 0; i < 3; ++i) { r[(size_t) i] = s.removeFromTop (bh); s.removeFromTop (gap); }
        return r;
    }

    struct Lfo
    {
        std::unique_ptr<HSelector> dest;
        std::unique_ptr<RotaryKnob> rate, depth;
        std::unique_ptr<ShapeSelector> shape;
    };
    std::array<Lfo, 3> lfos;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LfoSection)
};
