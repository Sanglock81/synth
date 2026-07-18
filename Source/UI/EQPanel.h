#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "VASynthLookAndFeel.h"
#include "PanelChrome.h"
#include "../PluginProcessor.h"
#include "../DSP/PartEQ.h"
#include <array>

// ============================================================================
// K1 — the ONE EQ. A fixed 4-band parametric EQ at the END of the focused part's
// chain (post-FX, applied last by FXChain). This section FOLLOWS EDIT FOCUS: it reads
// and writes the per-part peq_* params, which the processor keeps pointed at the
// focused part, so the header always names whose EQ this is.
//
// Interaction (no knobs — a mixing-desk feel):
//   * vertical drag on a band  = GAIN   (grab-relative; first touch doesn't jump)
//   * horizontal drag          = FREQ   (log sweep; live readout beneath the slider)
//   * double-tap a band        = numeric FREQ / GAIN / Q entry (pick from a menu)
//   * the dot at the top       = that band on/off (off = unity, click-free)
//   * tap the header bar        = the whole section on/off (peq_on)
// Editing any band auto-enables the section, so a boosted band is never silently
// bypassed. Focus-refusing so QWERTY note entry keeps working.
//
// NOTE: MIDI-learn/LINK on the EQ gains is done through the mod matrix (EqB1..B4Gain
// are registered dests / macro targets), not by arming the slider itself.
// ============================================================================

class EQPanel : public juce::Component,
                private juce::Timer
{
public:
    explicit EQPanel (VASynthProcessor& p) : proc (p)
    {
        setWantsKeyboardFocus (false);
        curveEq.prepare (48000.0);
        eqOn = dynamic_cast<juce::AudioParameterBool*> (proc.apvts.getParameter (ParamID::peqOn));
        startTimerHz (20);   // repaint when focus / values move, and animate the gain ghost
    }

    void paint (juce::Graphics& g) override
    {
        const auto tViz = juce::Colour (0xff67c0c8);
        auto content = chrome::section (g, getLocalBounds(), headerText(), tViz);

        // Section on/off bar — LOUD, so a disabled EQ never reads as broken.
        const bool on = eqOn != nullptr && eqOn->get();
        barR = content.removeFromTop (24); content.removeFromTop (4);
        g.setColour (on ? tViz : VASynthLookAndFeel::track().darker (0.35f));
        g.fillRoundedRectangle (barR.toFloat(), 4.0f);
        g.setColour (on ? tViz.brighter (0.5f) : tViz.withAlpha (0.55f));
        g.drawRoundedRectangle (barR.toFloat().reduced (0.8f), 4.0f, on ? 1.4f : 1.2f);
        g.setColour (on ? chrome::onTint() : tViz.withAlpha (0.9f));
        g.setFont (juce::Font (juce::FontOptions (12.5f, juce::Font::bold)));
        g.drawText (on ? "  EQ ON   -  tap to bypass" : "  EQ OFF   -  tap to enable",
                    barR, juce::Justification::centredLeft, false);

        auto foot = content.removeFromBottom (15);
        g.setColour (VASynthLookAndFeel::dim());
        g.setFont (juce::Font (juce::FontOptions (9.5f)));
        g.drawText ("drag = gain   sideways = freq   double-tap = value   dot = band",
                    foot, juce::Justification::centred, false);

        bodyR = content.reduced (6, 4);
        const int bw = bodyR.getWidth() / kNumBands;
        for (int i = 0; i < kNumBands; ++i)
        {
            auto col = bandColumn (i);
            const bool bon = bandOn (i);
            const auto colTint = (on && bon) ? tViz : VASynthLookAndFeel::dim();

            // on/off dot
            auto dot = col.removeFromTop (14);
            g.setColour (bon ? tViz : VASynthLookAndFeel::track().brighter (0.1f));
            g.fillEllipse (dot.withSizeKeepingCentre (10, 10).toFloat());

            // band name
            g.setColour (bon ? VASynthLookAndFeel::ink() : VASynthLookAndFeel::dim());
            g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
            g.drawText (bandName (i), col.removeFromTop (14), juce::Justification::centred, false);

            // gain readout (bottom) + freq label (above it)
            auto val = col.removeFromBottom (13);
            g.setColour (VASynthLookAndFeel::accent());
            g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 10.5f, juce::Font::bold)));
            g.drawText (gainText (i), val, juce::Justification::centred, false);
            auto flab = col.removeFromBottom (13);
            g.setColour (VASynthLookAndFeel::dim());
            g.setFont (juce::Font (juce::FontOptions (10.0f)));
            g.drawText (freqText (i), flab, juce::Justification::centred, false);

            // vertical gain slider (track + 0 dB centre + thumb + faint ghost while modulated)
            auto track = col.withSizeKeepingCentre (16, col.getHeight());
            g.setColour (VASynthLookAndFeel::track().darker (0.25f));
            g.fillRoundedRectangle (track.toFloat(), 5.0f);
            g.setColour (VASynthLookAndFeel::dim().withAlpha (0.5f));
            g.drawHorizontalLine (track.getCentreY(), (float) track.getX() - 3, (float) track.getRight() + 3);

            const float gN = gainNorm (i);                         // 0..1 (0.5 = 0 dB)
            const int   ty = thumbY (track, gN);
            g.setColour (colTint);
            g.fillRoundedRectangle (juce::Rectangle<int> (track.getX() - 5, ty, track.getWidth() + 10, 14).toFloat(), 3.0f);

            // modulation ghost thumb (motion-gated), if this gain is being modulated
            const float mod = proc.modAnimNorm (eqDestFor (i), proc.apvts.getParameter (gainId (i)));
            if (std::abs (mod) > 0.0025f)
            {
                const int my = thumbY (track, juce::jlimit (0.0f, 1.0f, gN + mod));
                g.setColour (colTint.withAlpha (0.5f));
                g.fillRoundedRectangle (juce::Rectangle<int> (track.getX() - 5, my, track.getWidth() + 10, 14).toFloat(), 3.0f);
            }
            (void) bw;
        }
    }

    void resized() override { /* paint owns geometry; numEditor positioned on open */ }

    // --- test hooks (geometry is computed in paint; snapshot once before using) ------
    juce::Point<int> testBandCentre (int b) { return bandColumn (b).getCentre (); }
    juce::Point<int> testBandDot (int b) { auto c = bandColumn (b); return c.removeFromTop (14).getCentre(); }
    juce::Point<int> testHeaderCentre() const { return barR.getCentre(); }

    // ---- gestures -----------------------------------------------------------
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (numEditor != nullptr) { closeNumeric(); return; }
        if (barR.contains (e.getPosition())) { headerArmed = true; return; }

        const int b = bandAt (e.getPosition());
        if (b < 0) return;
        auto col = bandColumn (b);
        dotArmed = col.removeFromTop (14).expanded (4, 4).contains (e.getPosition());
        if (dotArmed) { activeBand = b; return; }

        activeBand = b;
        axis = None;
        startGain = gainNorm (b);
        startFreq = freqNorm (b);
        beginGesture (b);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (activeBand < 0 || dotArmed || headerArmed) return;
        if (axis == None && e.getDistanceFromDragStart() > 4)
            axis = std::abs (e.getDistanceFromDragStartX()) > std::abs (e.getDistanceFromDragStartY())
                       ? Horizontal : Vertical;
        if (axis == Vertical)
        {
            const float dN = -(float) e.getDistanceFromDragStartY() / (float) juce::jmax (1, gainPixels());
            setNorm (gainId (activeBand), juce::jlimit (0.0f, 1.0f, startGain + dN));
            ensureEnabled();
        }
        else if (axis == Horizontal)
        {
            const float dN = (float) e.getDistanceFromDragStartX() / (float) kFreqPixels;
            setNorm (freqId (activeBand), juce::jlimit (0.0f, 1.0f, startFreq + dN));
            ensureEnabled();
        }
        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        const bool tap = e.getDistanceFromDragStart() <= 6;
        if (headerArmed && tap && barR.contains (e.getPosition()) && eqOn != nullptr)
        {
            eqOn->beginChangeGesture();
            eqOn->setValueNotifyingHost (eqOn->get() ? 0.0f : 1.0f);
            eqOn->endChangeGesture();
        }
        else if (dotArmed && tap && activeBand >= 0)
        {
            toggleBand (activeBand);
            ensureEnabled();
        }
        else if (activeBand >= 0 && axis != None)
            endGesture (activeBand);

        headerArmed = dotArmed = false;
        activeBand = -1; axis = None;
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int b = bandAt (e.getPosition());
        if (b < 0) return;
        juce::PopupMenu m;
        m.addSectionHeader (juce::String (bandName (b)) + " band");
        m.addItem (1, "Freq...");
        m.addItem (2, "Gain...");
        m.addItem (3, "Q...");
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
            [this, b] (int r)
            {
                if (r == 0) return;
                const char* id = r == 1 ? freqId (b) : r == 2 ? gainId (b) : qId (b);
                openNumeric (b, id);
            });
    }

private:
    enum Axis { None, Vertical, Horizontal };
    static constexpr int kNumBands = PartEQ::kNumBands;   // 4
    static constexpr int kFreqPixels = 260;               // px for a full 0..1 freq sweep

    // --- per-band param id helpers (the four peq bands, appended B4 in K1) -----
    static const char* freqId (int b) { const char* a[] { ParamID::peqB1Freq, ParamID::peqB2Freq, ParamID::peqB3Freq, ParamID::peqB4Freq }; return a[b]; }
    static const char* gainId (int b) { const char* a[] { ParamID::peqB1Gain, ParamID::peqB2Gain, ParamID::peqB3Gain, ParamID::peqB4Gain }; return a[b]; }
    static const char* qId    (int b) { const char* a[] { ParamID::peqB1Q,    ParamID::peqB2Q,    ParamID::peqB3Q,    ParamID::peqB4Q    }; return a[b]; }
    static const char* onId   (int b) { const char* a[] { ParamID::peqB1On,   ParamID::peqB2On,   ParamID::peqB3On,   ParamID::peqB4On   }; return a[b]; }
    static int eqDestFor (int b) { const int d[] { ModMatrix::EqB1Gain, ModMatrix::EqB2Gain, ModMatrix::EqB3Gain, ModMatrix::EqB4Gain }; return d[b]; }
    static const char* bandName (int b) { const char* a[] { "LOW", "L-MID", "H-MID", "HIGH" }; return a[b]; }

    juce::String headerText() const
    {
        const int part = proc.editFocus();
        juce::String name = "EQ  -  P" + juce::String (part + 1);
        if (part == 0) name += "  LIVE";
        else           name += "  " + proc.getPartPreset (part);
        return name;
    }

    // --- param read helpers ---------------------------------------------------
    float rawOf (const char* id) const { return proc.apvts.getRawParameterValue (id)->load(); }
    float gainNorm (int b) const { auto* p = proc.apvts.getParameter (gainId (b)); return p ? p->getValue() : 0.5f; }
    float freqNorm (int b) const { auto* p = proc.apvts.getParameter (freqId (b)); return p ? p->getValue() : 0.5f; }
    bool  bandOn   (int b) const { return rawOf (onId (b)) > 0.5f; }

    juce::String gainText (int b) const
    {
        const float db = rawOf (gainId (b));
        return juce::String (db >= 0 ? "+" : "") + juce::String (db, 1) + " dB";
    }
    juce::String freqText (int b) const
    {
        const float f = rawOf (freqId (b));
        return f >= 1000.0f ? juce::String (f / 1000.0f, 1) + " kHz" : juce::String ((int) f) + " Hz";
    }

    // --- geometry -------------------------------------------------------------
    juce::Rectangle<int> bandColumn (int b) const
    {
        const int bw = bodyR.getWidth() / kNumBands;
        return juce::Rectangle<int> (bodyR.getX() + b * bw, bodyR.getY(), bw, bodyR.getHeight()).reduced (3, 0);
    }
    int bandAt (juce::Point<int> pt) const
    {
        if (! bodyR.contains (pt)) return -1;
        const int bw = juce::jmax (1, bodyR.getWidth() / kNumBands);
        return juce::jlimit (0, kNumBands - 1, (pt.x - bodyR.getX()) / bw);
    }
    // The slider track lives between the name (top 14+14) and the two bottom labels (13+13).
    int gainPixels() const { return juce::jmax (10, bodyR.getHeight() - 14 - 14 - 13 - 13 - kThumbH); }
    static constexpr int kThumbH = 14;
    static int thumbY (juce::Rectangle<int> track, float norm)
    {
        return track.getY() + (int) ((1.0f - norm) * (float) juce::jmax (1, track.getHeight() - kThumbH));
    }

    // --- param write ----------------------------------------------------------
    void setNorm (const char* id, float n) { if (auto* p = proc.apvts.getParameter (id)) p->setValueNotifyingHost (n); }
    void beginGesture (int b) { if (auto* p = proc.apvts.getParameter (axisId (b))) p->beginChangeGesture(); }
    void endGesture   (int b) { if (auto* p = proc.apvts.getParameter (axisId (b))) p->endChangeGesture(); }
    const char* axisId (int b) const { return axis == Horizontal ? freqId (b) : gainId (b); }

    void toggleBand (int b)
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*> (proc.apvts.getParameter (onId (b))))
        {
            p->beginChangeGesture(); p->setValueNotifyingHost (p->get() ? 0.0f : 1.0f); p->endChangeGesture();
        }
    }
    void ensureEnabled()
    {
        if (eqOn != nullptr && ! eqOn->get())
        {
            eqOn->beginChangeGesture(); eqOn->setValueNotifyingHost (1.0f); eqOn->endChangeGesture();
        }
    }

    // --- numeric entry --------------------------------------------------------
    // Lazy: the editor exists ONLY while the user is entering a value, so the resting
    // component tree has no keyboard-focus-wanting child (QWERTY-never-starved invariant).
    void openNumeric (int b, const char* id)
    {
        numParam = proc.apvts.getParameter (id);
        if (numParam == nullptr) return;
        numEditor = std::make_unique<juce::TextEditor>();
        numEditor->setMultiLine (false);
        numEditor->setJustification (juce::Justification::centred);
        numEditor->setColour (juce::TextEditor::backgroundColourId, VASynthLookAndFeel::panelLight());
        numEditor->setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff67c0c8));
        numEditor->onReturnKey = [this] { commitNumeric(); };
        numEditor->onEscapeKey = [this] { closeNumeric(); };
        numEditor->onFocusLost = [this] { commitNumeric(); };
        auto col = bandColumn (b);
        numEditor->setBounds (col.getCentreX() - 34, col.getCentreY() - 10, 68, 20);
        numEditor->setText (numParam->getCurrentValueAsText().upToFirstOccurrenceOf (" ", false, false), false);
        addAndMakeVisible (*numEditor);
        numEditor->grabKeyboardFocus();
        numEditor->selectAll();
    }
    void commitNumeric()
    {
        if (numParam != nullptr && numEditor != nullptr)
        {
            const float v = numEditor->getText().getFloatValue();
            const auto& r = numParam->getNormalisableRange();
            numParam->beginChangeGesture();
            numParam->setValueNotifyingHost (r.convertTo0to1 (juce::jlimit (r.start, r.end, v)));
            numParam->endChangeGesture();
            ensureEnabled();
        }
        closeNumeric();
    }
    void closeNumeric() { numEditor.reset(); numParam = nullptr; repaint(); }

    void timerCallback() override { repaint(); }

    VASynthProcessor& proc;
    PartEQ curveEq;                                  // reserved for a future response curve
    juce::AudioParameterBool* eqOn = nullptr;
    juce::Rectangle<int> barR, bodyR;

    int  activeBand = -1;
    Axis axis = None;
    float startGain = 0.5f, startFreq = 0.5f;
    bool headerArmed = false, dotArmed = false;

    std::unique_ptr<juce::TextEditor> numEditor;
    juce::RangedAudioParameter* numParam = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EQPanel)
};
