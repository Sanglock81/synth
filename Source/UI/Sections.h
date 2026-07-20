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
        const char* phIds[]   { ID::osc1Phase, ID::osc2Phase, ID::osc3Phase };
        const juce::StringArray waveLabels { "SAW", "SQR", "TRI", "SIN" };
        const juce::StringArray phaseLabels { "RST", "RND", "FRE" };   // Tier 1a start-phase policy

        for (int i = 0; i < 3; ++i)
        {
            auto& o = oscs[(size_t) i];
            o.on   = std::make_unique<PowerToggle> (p.apvts, onIds[i], "ON");
            o.wave = std::make_unique<HSelector> (p.apvts, waveIds[i], p.getMidiLearn(), waveLabels);
            o.phase = std::make_unique<HSelector> (p.apvts, phIds[i], p.getMidiLearn(), phaseLabels);
            o.k[0] = std::make_unique<RotaryKnob> (p.apvts, octIds[i], "OCTAVE", p.getMidiLearn());
            o.k[1] = std::make_unique<RotaryKnob> (p.apvts, detIds[i], "DETUNE", p.getMidiLearn());
            o.k[2] = std::make_unique<RotaryKnob> (p.apvts, pwIds[i],  "PW",     p.getMidiLearn());
            o.k[3] = std::make_unique<RotaryKnob> (p.apvts, lvlIds[i], "LEVEL",  p.getMidiLearn());
            // LINK targets + animation are wired centrally from the registry (editor::wireModTargets);
            // PW/level/cutoff/reso etc. no longer need per-knob wiring here.
            addAndMakeVisible (*o.on);   addAndMakeVisible (*o.wave); addAndMakeVisible (*o.phase);
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
            o.phase->setBounds (top.removeFromRight (82)); top.removeFromRight (5);   // Tier 1a phase policy
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
        std::unique_ptr<HSelector> wave, phase;
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
            // LINK target + animation for cutoff/reso/env-amt/keytrack/vel are wired centrally
            // from the registry (editor::wireModTargets) — no per-knob wiring here.
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
        const char* syncIds[]  { ID::lfoSync,  ID::lfo2Sync,  ID::lfo3Sync  };
        const char* divIds[]   { ID::lfoDiv,   ID::lfo2Div,   ID::lfo3Div   };
        const juce::StringArray destLabels { "OFF", "PITCH", "CUTOFF", "PW" };

        for (int i = 0; i < 3; ++i)
        {
            auto& l = lfos[(size_t) i];
            l.dest  = std::make_unique<HSelector> (p.apvts, destIds[i], p.getMidiLearn(), destLabels);
            l.rate  = std::make_unique<RotaryKnob> (p.apvts, rateIds[i],  "RATE",  p.getMidiLearn());
            l.div   = std::make_unique<RotaryKnob> (p.apvts, divIds[i],   "DIV",   p.getMidiLearn());
            l.depth = std::make_unique<RotaryKnob> (p.apvts, depthIds[i], "DEPTH", p.getMidiLearn());
            l.shape = std::make_unique<ShapeSelector> (p.apvts, shapeIds[i], p.getMidiLearn());
            l.sync  = std::make_unique<PowerToggle> (p.apvts, syncIds[i], "SYNC");
            addAndMakeVisible (*l.dest);  addAndMakeVisible (*l.rate);
            addChildComponent (*l.div);   // shown only when SYNC is on (swaps with RATE)
            addAndMakeVisible (*l.depth); addAndMakeVisible (*l.shape);
            addAndMakeVisible (*l.sync);

            // SYNC toggle morphs the RATE knob (free Hz) <-> DIV knob (note division).
            auto* syncParam = p.apvts.getParameter (syncIds[i]);
            l.syncAtt = std::make_unique<juce::ParameterAttachment> (
                *syncParam, [this, i] (float v) { applySyncMode (i, v > 0.5f); });
            l.syncAtt->sendInitialUpdate();
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
            auto top = c.removeFromTop (26); c.removeFromTop (4);
            l.sync->setBounds (top.removeFromRight (46)); top.removeFromRight (5);
            l.dest->setBounds (top);
            l.shape->setBounds (c.removeFromRight (40)); c.removeFromRight (5);
            auto rateSlot = c.removeFromLeft (c.getWidth() / 2).reduced (2, 0);
            l.rate->setBounds (rateSlot);           // RATE and DIV share one slot; SYNC picks which
            l.div->setBounds (rateSlot);
            l.depth->setBounds (c.reduced (2, 0));
        }
    }

private:
    // SYNC on -> show DIV (note division), hide RATE (free Hz); off -> the reverse.
    void applySyncMode (int i, bool synced)
    {
        auto& l = lfos[(size_t) i];
        if (l.rate) l.rate->setVisible (! synced);
        if (l.div)  l.div->setVisible (synced);
    }

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
        std::unique_ptr<RotaryKnob> rate, div, depth;
        std::unique_ptr<ShapeSelector> shape;
        std::unique_ptr<PowerToggle> sync;
        std::unique_ptr<juce::ParameterAttachment> syncAtt;
    };
    std::array<Lfo, 3> lfos;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LfoSection)
};
