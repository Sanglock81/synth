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

// A compact die: one tap re-rolls a random wavetable (#95 3c). Draws a 5-pip die face so it reads
// as "randomize" without depending on a glyph font, and doesn't fight the MIDI-learn long-press.
class DieButton : public juce::Button
{
public:
    DieButton() : juce::Button ("die") { setWantsKeyboardFocus (false); }
    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.5f);
        const auto base = VASynthLookAndFeel::track();
        g.setColour (down ? VASynthLookAndFeel::accent()
                          : (over ? base.brighter (0.35f) : base.brighter (0.15f)));
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (VASynthLookAndFeel::ink().withAlpha (0.6f));
        g.drawRoundedRectangle (r, 3.0f, 1.0f);
        const float cx = r.getCentreX(), cy = r.getCentreY();
        const float dx = r.getWidth() * 0.26f, dy = r.getHeight() * 0.26f;
        const float pr = juce::jmax (1.0f, r.getWidth() * 0.09f);
        auto pip = [&] (float x, float y) { g.fillEllipse (x - pr, y - pr, pr * 2.0f, pr * 2.0f); };
        g.setColour (down ? juce::Colours::black : VASynthLookAndFeel::ink());
        pip (cx - dx, cy - dy); pip (cx + dx, cy - dy);
        pip (cx, cy);
        pip (cx - dx, cy + dy); pip (cx + dx, cy + dy);
    }
};

// ---------------------------------------------------------------------------
// OSCILLATORS: one sub-box per osc — [ON | wave selector] across the top, then
// OCTAVE / DETUNE / PW / LEVEL knobs.
class OscSection : public juce::Component
{
public:
    static constexpr int kPwDragPixels = 150;   // #2: snappier than the 313-px default (narrow audible range)

    explicit OscSection (VASynthProcessor& p) : proc (p)
    {
        namespace ID = ParamID;
        const char* onIds[]   { ID::osc1On, ID::osc2On, ID::osc3On };
        const char* waveIds[] { ID::osc1Wave, ID::osc2Wave, ID::osc3Wave };
        const char* wtPosIds[]  { ID::osc1WtPos,  ID::osc2WtPos,  ID::osc3WtPos  };
        const char* octIds[]  { ID::osc1Octave, ID::osc2Octave, ID::osc3Octave };
        const char* semiIds[] { ID::osc1Semi, ID::osc2Semi, ID::osc3Semi };
        const char* detIds[]  { ID::osc1Detune, ID::osc2Detune, ID::osc3Detune };
        const char* pwIds[]   { ID::osc1PW, ID::osc2PW, ID::osc3PW };
        const char* lvlIds[]  { ID::osc1Level, ID::osc2Level, ID::osc3Level };
        const char* fmIds[]   { ID::osc1Fm, ID::osc2Fm, nullptr };   // #132 FM on osc1/osc2 rows only (osc3 tops the chain)
        // Label states the chain direction plainly: "FM 2>1" = osc 2 phase-modulates osc 1.
        const char* fmName[]  { "FM 2>1", "FM 3>2", nullptr };
        const char* phIds[]   { ID::osc1Phase, ID::osc2Phase, ID::osc3Phase };
        const juce::StringArray waveLabels { "SAW", "SQR", "TRI", "SIN", "WT" };
        const juce::StringArray phaseLabels { "RST", "RND", "FRE" };   // Tier 1a start-phase policy

        for (int i = 0; i < 3; ++i)
        {
            auto& o = oscs[(size_t) i];
            o.on   = std::make_unique<PowerToggle> (p.apvts, onIds[i], "ON");
            o.wave = std::make_unique<HSelector> (p.apvts, waveIds[i], p.getMidiLearn(), waveLabels);
            o.phase = std::make_unique<HSelector> (p.apvts, phIds[i], p.getMidiLearn(), phaseLabels);
            o.phase->setHelp ("Start phase per note: RST reset / RND random / FRE free-run");
            o.k[0] = std::make_unique<RotaryKnob> (p.apvts, octIds[i],  "OCTAVE", p.getMidiLearn());
            o.k[1] = std::make_unique<RotaryKnob> (p.apvts, semiIds[i], "SEMI",   p.getMidiLearn());
            o.k[1]->setHelp ("Coarse tune this oscillator in semitones (-24..+24) — stack intervals like a fifth");
            o.k[2] = std::make_unique<RotaryKnob> (p.apvts, detIds[i], "DETUNE", p.getMidiLearn());
            o.k[3] = std::make_unique<RotaryKnob> (p.apvts, pwIds[i],  "PW",     p.getMidiLearn());
            // #2: PW's audible sweet spot is narrow, so the default 313-px full-range drag felt
            // sluggish. Halve the drag distance for a snappier, more responsive PW knob.
            o.k[3]->setDragPixels (kPwDragPixels);
            o.k[4] = std::make_unique<RotaryKnob> (p.apvts, lvlIds[i], "LEVEL",  p.getMidiLearn());
            if (fmIds[i] != nullptr)   // #132 FM depth knob (osc1/osc2); enabled state tracks the carrier wave
            {
                o.fm = std::make_unique<RotaryKnob> (p.apvts, fmIds[i], fmName[i], p.getMidiLearn());
                o.fm->setHelp (fmEnabledHelp (i));
                // Repaint the panel when this depth crosses 0 so the modulator-row "MOD" badge tracks it.
                o.fmAtt = std::make_unique<juce::ParameterAttachment> (
                    *p.apvts.getParameter (fmIds[i]), [this] (float) { repaint(); });
            }
            // #95 3c: a WT POS knob shares the PW slot (k[3]) — PW is meaningless for a wavetable.
            // A ParameterAttachment on the wave choice swaps which of the two is visible (the same
            // same-bounds morph idiom as the LFO RATE<->DIV knob and the EnvSection AMP/MOD swap).
            o.wtpos = std::make_unique<RotaryKnob> (p.apvts, wtPosIds[i], "WT POS", p.getMidiLearn());
            o.wtpos->setHelp ("Wavetable position: morphs through the table's frames");
            o.die = std::make_unique<DieButton>();   // one-tap re-roll of a random table (WT mode only)
            o.die->setTooltip ("Re-roll a random wavetable (tap the WT label to choose a factory table)");
            o.die->onClick = [this, i] { rollRandomTable (i); };
            // LINK targets + animation are wired centrally from the registry (editor::wireModTargets);
            // PW/level/cutoff/reso etc. no longer need per-knob wiring here.
            addAndMakeVisible (*o.on);   addAndMakeVisible (*o.wave); addAndMakeVisible (*o.phase);
            for (auto& k : o.k) addAndMakeVisible (*k);
            if (o.fm) addAndMakeVisible (*o.fm);
            addChildComponent (*o.wtpos);   // shown only when this osc's wave == WT (swaps with PW)
            addChildComponent (*o.die);     // shown only in WT mode (the re-roll affordance)

            // Second tap on the WT wave button (when already selected) opens the table picker.
            o.wave->onReselect = [this, i] (int idx) { if (idx == kWtWaveIndex) openTablePicker (i); };
            // The wave choice morphs the PW<->WT POS knob (WT hides PW, shows WT POS).
            auto* waveParam = p.apvts.getParameter (waveIds[i]);
            o.waveAtt = std::make_unique<juce::ParameterAttachment> (
                *waveParam, [this, i] (float) { applyWaveMode (i); });
            o.waveAtt->sendInitialUpdate();
        }

        // NOISE — the 4th sound source (white noise), given the SAME row anatomy as an oscillator
        // at slim height: a tinted "NOISE" source label on the left (where the osc ON/wave headers
        // sit) and a LEVEL knob aligned under the three oscillator LEVEL knobs (column alignment is
        // what makes it read as part of the mixer, not an orphan). The open middle is reserved for
        // the post-1.0 noise COLOR selector (white / pink) — see docs/roadmap.
        noise = std::make_unique<HBarControl> (p.apvts, ID::noiseLevel, "NOISE", p.getMidiLearn());
        noise->setHelp ("White-noise source level (the 4th sound source) — drag the bar left/right");
        addAndMakeVisible (*noise);
    }

    void paint (juce::Graphics& g) override
    {
        chrome::section (g, getLocalBounds(), "Oscillators", sectiontint::osc());
        auto boxes = boxRects();
        for (auto& b : boxes) chrome::subBox (g, b, sectiontint::osc());
        chrome::subBox (g, noiseBox(), sectiontint::osc());

        // #132 FM: badge the row of an oscillator that is currently a MODULATOR (its carrier's FM
        // depth > 0) so the osc2->osc1 / osc3->osc2 relationship is visible on the panel — an accent
        // outline plus a small "MOD>OSC n" chip riding the row's top edge, pointing at the carrier it
        // feeds. osc2 modulates osc1; osc3 modulates osc2.
        for (int i = 0; i < 3; ++i)
            if (isActiveModulator (i))
            {
                auto box = boxes[(size_t) i];
                g.setColour (VASynthLookAndFeel::accent().withAlpha (0.85f));
                g.drawRoundedRectangle (box.toFloat().reduced (1.0f), 6.0f, 1.6f);
                juce::Rectangle<int> chip (box.getRight() - 86, box.getY() - 7, 78, 11);
                g.setColour (VASynthLookAndFeel::accent());
                g.fillRoundedRectangle (chip.toFloat(), 4.0f);
                g.setColour (juce::Colours::black.withAlpha (0.85f));
                g.setFont (juce::Font (juce::FontOptions (9.5f, juce::Font::bold)));
                g.drawText (i == 1 ? "MOD > OSC 1" : "MOD > OSC 2", chip, juce::Justification::centred, false);
            }

        // "NOISE" source label (left), tinted like the osc rows — the 4th mixer source.
        auto nb = chrome::subBoxContent (noiseBox());
        auto lbl = nb.removeFromLeft (juce::jmin (76, nb.getWidth() / 2));
        g.setColour (sectiontint::osc().withAlpha (0.22f));
        g.fillRoundedRectangle (lbl.toFloat().reduced (1.0f), 4.0f);
        g.setColour (sectiontint::osc().brighter (0.35f));
        g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
        g.drawText ("NOISE", lbl.reduced (8, 0), juce::Justification::centredLeft, false);
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
            // #95 3c: a reserved die slot sits just right of the wave selector (the picker), shown only
            // in WT mode. Reserved always (not carved on mode change) so nothing shifts between modes.
            o.die->setBounds (top.removeFromRight (22).reduced (0, 3)); top.removeFromRight (4);
            o.wave->setBounds (top);
            // Six columns: OCTAVE | SEMI | DETUNE | FM | PW(/WT POS) | LEVEL. FM sits between DETUNE and
            // PW on osc1/osc2; osc3 tops the chain so its FM column stays empty (LEVEL stays rightmost,
            // keeping the column alignment — incl. the NOISE bar under LEVEL — across all three rows).
            const int kw = c.getWidth() / 6;
            o.k[0]->setBounds (c.removeFromLeft (kw).reduced (2, 0));   // OCTAVE
            o.k[1]->setBounds (c.removeFromLeft (kw).reduced (2, 0));   // SEMI
            o.k[2]->setBounds (c.removeFromLeft (kw).reduced (2, 0));   // DETUNE
            auto fmCol = c.removeFromLeft (kw);                         // FM (osc3: reserved + empty)
            if (o.fm) o.fm->setBounds (fmCol.reduced (2, 0));
            o.k[3]->setBounds (c.removeFromLeft (kw).reduced (2, 0));   // PW
            o.k[4]->setBounds (c.reduced (2, 0));                       // LEVEL
            o.wtpos->setBounds (o.k[3]->getBounds());   // shares the PW slot (visibility swaps)
        }
        // NOISE fill-bar: fills the row to the right of the "NOISE" source label (the open middle
        // between them is the reserved post-1.0 COLOR-selector slot). The bar itself shows the level.
        auto nc = chrome::subBoxContent (noiseBox());
        nc.removeFromLeft (juce::jmin (76, nc.getWidth() / 2) + 10);   // clear the NOISE label + a gap
        noise->setBounds (nc.reduced (2, 12));
    }

private:
    static constexpr int kNoiseStrip   = 46;   // compact 4th-source row height
    static constexpr int kWtWaveIndex  = 4;    // wave choice index of "WT" (5th option)

    static const char* waveId (int osc)
    { namespace ID = ParamID; const char* ids[] { ID::osc1Wave,   ID::osc2Wave,   ID::osc3Wave   }; return ids[(size_t) osc]; }
    static const char* wtKindId (int osc)
    { namespace ID = ParamID; const char* ids[] { ID::osc1WtKind, ID::osc2WtKind, ID::osc3WtKind }; return ids[(size_t) osc]; }
    static const char* wtSeedId (int osc)
    { namespace ID = ParamID; const char* ids[] { ID::osc1WtSeed, ID::osc2WtSeed, ID::osc3WtSeed }; return ids[(size_t) osc]; }

    // WT hides the (meaningless) PW knob and shows the WT POS knob on the same slot.
    void applyWaveMode (int osc)
    {
        auto* wave = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter (waveId (osc)));
        const bool wt = wave != nullptr && wave->getIndex() == kWtWaveIndex;
        auto& o = oscs[(size_t) osc];
        if (o.k[3])  o.k[3]->setVisible (! wt);
        if (o.wtpos) o.wtpos->setVisible (wt);
        if (o.die)   o.die->setVisible (wt);
        // #132 FM carrier restriction: FM applies only to a continuous-phase carrier (TRI/SIN/WT).
        // On a saw/square carrier the depth is inert in the DSP — disable + dim the knob and say why.
        if (o.fm && wave != nullptr)
        {
            const int wi = wave->getIndex();
            const bool carrier = wi == 2 || wi == 3 || wi == 4;   // TRI / SIN / WT
            o.fm->setEnabled (carrier);
            o.fm->setAlpha (carrier ? 1.0f : 0.4f);
            o.fm->setHelp (carrier ? fmEnabledHelp (osc) : fmDisabledHelp());
        }
    }

    // One-sentence tooltips that teach the model: WHERE the character comes from (the modulator's
    // pitch) and WHY a carrier can be disabled (the PolyBLEP restriction), in plain words.
    static juce::String fmEnabledHelp (int osc)
    {
        return osc == 0 ? "Osc 2 bends this oscillator's phase (FM); set depth here, character via Osc 2's SEMI/OCTAVE"
                        : "Osc 3 bends this oscillator's phase (FM); set depth here, character via Osc 3's SEMI/OCTAVE";
    }
    static juce::String fmDisabledHelp()
    {
        return "FM carrier must be SIN / TRI / WT; SAW and SQR break under phase modulation";
    }

    // #132: is this oscillator currently acting as an FM MODULATOR? Osc i modulates osc i-1 when
    // osc(i-1)'s FM depth is above 0 (osc2 -> osc1 via osc1_fm; osc3 -> osc2 via osc2_fm).
    bool isActiveModulator (int osc) const
    {
        namespace ID = ParamID;
        const char* carrierFmId = osc == 1 ? ID::osc1Fm : osc == 2 ? ID::osc2Fm : nullptr;
        if (carrierFmId == nullptr) return false;
        if (auto* v = proc.apvts.getRawParameterValue (carrierFmId)) return v->load() > 1.0e-4f;
        return false;
    }

    // Second tap on a selected WT button -> pick a factory table or (re-)roll a random one.
    void openTablePicker (int osc)
    {
        auto* kind = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter (wtKindId (osc)));
        auto* seed = dynamic_cast<juce::AudioParameterInt*>    (proc.apvts.getParameter (wtSeedId (osc)));
        if (kind == nullptr || seed == nullptr) return;
        const int  curKind  = kind->getIndex();
        const bool isRandom = seed->get() > 0;
        juce::PopupMenu m;
        m.addSectionHeader ("Wavetable");
        for (int t = 0; t < kind->choices.size(); ++t)
            m.addItem (t + 1, kind->choices[t], true, ! isRandom && t == curKind);   // factory table (clears the seed)
        m.addSeparator();
        m.addItem (100, juce::String (isRandom ? "Random (re-roll)" : "Random"), true, isRandom);   // the die
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (oscs[(size_t) osc].wave.get()),
            [this, osc] (int r)
            {
                if (r <= 0) return;
                if (r == 100) rollRandomTable (osc);
                else          selectFactoryTable (osc, r - 1);
            });
    }

    // Pick factory table k: clear the seed (0 -> factory path) and set the kind.
    void selectFactoryTable (int osc, int k)
    {
        setWtSeed (osc, 0);
        if (auto* kind = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter (wtKindId (osc))))
        {
            kind->beginChangeGesture();
            kind->setValueNotifyingHost (kind->convertTo0to1 ((float) k));
            kind->endChangeGesture();
        }
    }
    // The die: a fresh seed (>0) selects a new deterministic random table for this osc.
    void rollRandomTable (int osc)
    {
        setWtSeed (osc, 1 + juce::Random::getSystemRandom().nextInt (1000000));
    }
    // Exact integer set (avoids any normalized round-trip); notifies the host -> processor rebuilds.
    void setWtSeed (int osc, int s)
    {
        if (auto* seed = dynamic_cast<juce::AudioParameterInt*> (proc.apvts.getParameter (wtSeedId (osc))))
        {
            seed->beginChangeGesture();
            *seed = s;
            seed->endChangeGesture();
        }
    }

    std::array<juce::Rectangle<int>, 3> boxRects() const
    {
        auto s = chrome::sectionContent (getLocalBounds());
        s.removeFromBottom (kNoiseStrip + 5);                       // reserve the NOISE strip + a gap
        const int gap = 5, bh = juce::jmax (10, (s.getHeight() - 2 * gap) / 3);
        std::array<juce::Rectangle<int>, 3> r;
        for (int i = 0; i < 3; ++i) { r[(size_t) i] = s.removeFromTop (bh); s.removeFromTop (gap); }
        return r;
    }

    juce::Rectangle<int> noiseBox() const
    {
        return chrome::sectionContent (getLocalBounds()).removeFromBottom (kNoiseStrip);
    }

    struct Osc
    {
        std::unique_ptr<PowerToggle> on;
        std::unique_ptr<HSelector> wave, phase;
        std::array<std::unique_ptr<RotaryKnob>, 5> k;   // OCTAVE, SEMI, DETUNE, PW, LEVEL
        std::unique_ptr<RotaryKnob> fm;                       // #132 FM depth (osc1/osc2 rows only; carrier = this osc)
        std::unique_ptr<juce::ParameterAttachment> fmAtt;     // #132 repaint the MOD badge when depth crosses 0
        std::unique_ptr<RotaryKnob> wtpos;                    // shares the PW slot; visible only in WT mode
        std::unique_ptr<DieButton>  die;                      // WT-mode re-roll affordance (right of the picker)
        std::unique_ptr<juce::ParameterAttachment> waveAtt;  // morphs PW <-> WT POS on wave change
    };
    std::array<Osc, 3> oscs;
    std::unique_ptr<HBarControl> noise;   // 4th source: white-noise level (horizontal fill bar)
    VASynthProcessor& proc;               // for the WT table picker (reads/sets osc*_wt_kind)

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

        struct KD { const char* pid; const char* name; const char* help; };
        const KD kd[] {
            { ID::filterCutoff,   "CUTOFF",  nullptr },
            { ID::filterReso,     "RESO",    "Resonance. Past the top it self-oscillates into a sine at cutoff" },
            { ID::filterDrive,    "DRIVE",   "Filter drive: analog-style saturation inside the filter loop" },
            { ID::filterEnvAmt,   "ENV AMT", nullptr },
            { ID::filterKeytrack, "KEYTRK",  nullptr },
            { ID::velToCutoff,    "VEL>CUT", nullptr } };
        for (auto& d : kd)
        {
            auto* k = new RotaryKnob (p.apvts, d.pid, d.name, p.getMidiLearn(), /*sideLabel*/ true);
            if (d.help != nullptr) k->setHelp (d.help);
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
            amp[i]->setTransientReadout (true); mod[i]->setTransientReadout (true);   // B3: value only while touched
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
        const juce::StringArray destLabels { "OFF", "PITCH", "CUTOFF", "ON" };

        for (int i = 0; i < 3; ++i)
        {
            auto& l = lfos[(size_t) i];
            l.dest  = std::make_unique<HSelector> (p.apvts, destIds[i], p.getMidiLearn(), destLabels);
            // #148 LFO Link mode: long-press this DEST arms/commits the link; a quick tap while armed
            // cancels. (Discoverability lives in the guide — the tooltip stays the param name.)
            l.dest->onLongPressOverride = [this, procPtr = &p, i]
            {
                if (procPtr->lfoLinkModeActive())
                { if (procPtr->lfoLinkModeLfo() != i) return false;   // a different LFO is armed
                  procPtr->commitLfoLinkMode(); }
                else procPtr->beginLfoLinkMode (i);
                if (auto* top = getTopLevelComponent()) top->repaint();
                return true;
            };
            l.dest->onShortPressOverride = [this, procPtr = &p]
            {
                if (! procPtr->lfoLinkModeActive()) return false;
                procPtr->cancelLfoLinkMode();
                if (auto* top = getTopLevelComponent()) top->repaint();
                return true;
            };
            l.rate  = std::make_unique<RotaryKnob> (p.apvts, rateIds[i],  "RATE",  p.getMidiLearn());
            l.div   = std::make_unique<RotaryKnob> (p.apvts, divIds[i],   "DIV",   p.getMidiLearn());
            l.div->setHelp ("Note division of the LFO when SYNC is on (1/4, 1/8, ...)");
            l.depth = std::make_unique<RotaryKnob> (p.apvts, depthIds[i], "DEPTH", p.getMidiLearn());
            l.shape = std::make_unique<ShapeSelector> (p.apvts, shapeIds[i], p.getMidiLearn());
            l.sync  = std::make_unique<PowerToggle> (p.apvts, syncIds[i], "SYNC");
            l.sync->setHelp ("Lock the LFO rate to tempo (RATE knob becomes a note-division selector)");
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
