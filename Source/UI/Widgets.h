// Synth — Copyright © 2026 John L Farmer. Licensed under AGPLv3; see LICENSE.
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "VASynthLookAndFeel.h"
#include "ModLink.h"
#include "ModAnim.h"
#include "../MidiLearnManager.h"
#include "../DSP/NoiseShaper.h"   // the noise field's own mappings — the readout must not re-derive them

// ============================================================================
// Touch-first, MIDI-learnable UI widgets bound to APVTS parameters. The APVTS
// attachment classes keep GUI <-> APVTS <-> DAW automation <-> MIDI-learn all in
// sync with no custom glue. Every widget refuses keyboard focus so the QWERTY
// computer-keyboard note input keeps working while twisting controls.
//
// MIDI-learn: right-click (mouse) or long-press (touch) any control -> arms
// MidiLearnManager for it (pulsing amber outline); the next incoming CC binds it
// and a small "CCnn" badge appears. The same gesture offers clear-mapping.
// ============================================================================

// R2 touch: pixels of finger travel to move a fader/knob across its FULL range
// (JUCE's Slider default is 250). Higher = less sensitive, so small movements don't
// over-shoot values during live play. Applied consistently to every fader + knob.
// ONE knob to re-tune after the layout rebuild changes control sizes (larger controls
// want more travel). ~313 = JUCE's 250 x 1.25 (~25% more travel / ~20% less per pixel).
inline constexpr int kDragPixelsForFullRange = 313;

// Clean, arm's-length-legible value readout for a float parameter: sensible
// decimal places for the magnitude, plus the parameter's own unit label (Hz, s,
// ms, ct) — JUCE's getCurrentValueAsText() shows neither (it dumps full float
// precision like "299.9999695" and never appends the label). Purely cosmetic;
// the parameter value/state is untouched.
inline juce::String formatParamValue (juce::RangedAudioParameter* p)
{
    if (auto* fp = dynamic_cast<juce::AudioParameterFloat*> (p))
    {
        const float v = fp->get();
        const float a = std::abs (v);
        juce::String num = a >= 100.0f ? juce::String (juce::roundToInt (v))
                         : a >= 10.0f  ? juce::String (v, 1)
                         : a >= 1.0f   ? juce::String (v, 2)
                                       : juce::String (v, 3);
        const juce::String unit = fp->getLabel();
        return unit.isEmpty() ? num : num + " " + unit;
    }
    return p != nullptr ? p->getCurrentValueAsText() : juce::String();
}

// Shared MIDI-learn interaction for a control bound to one parameter.
class LearnableComponent : public juce::Component,
                           public juce::SettableTooltipClient,
                           private juce::Timer
{
public:
    LearnableComponent (MidiLearnManager& learnToUse, juce::String paramIdToUse)
        : learn (learnToUse), paramID (std::move (paramIdToUse))
    {
        setWantsKeyboardFocus (false);
    }

    // Use the parameter's full registered name (e.g. "Filter Cutoff") as the hover tooltip.
    // `inner` is the interactive child that actually receives the mouse (a slider) — it needs its
    // own tooltip since the TooltipWindow reads the leaf under the cursor, not this parent.
    void setTooltipFromParam (juce::AudioProcessorValueTreeState& apvts, juce::Component* inner = nullptr)
    {
        if (auto* p = apvts.getParameter (paramID))
        {
            setTooltip (p->getName (128));
            if (auto* ttc = dynamic_cast<juce::SettableTooltipClient*> (inner)) ttc->setTooltip (getTooltip());
        }
    }

    // Override the hover tooltip with a self-explanatory description (for cryptic controls:
    // phase RS/RN/FR, ANALOG, DRIVE, LFO SYNC/DIV, ...). `inner` is the leaf the TooltipWindow
    // reads under the cursor — pass the interactive child (a slider) so it carries the text too.
    void setHelp (const juce::String& text, juce::Component* inner = nullptr)
    {
        setTooltip (text);
        if (auto* ttc = dynamic_cast<juce::SettableTooltipClient*> (inner)) ttc->setTooltip (text);
    }

    // Call from a subclass ctor after the inner control is added, passing it so
    // long-press/right-click on the control is captured here too.
    void listenForLearnGestures (juce::Component& inner)
    {
        inner.addMouseListener (this, true);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        // LINK routing takes priority (whole cell): if a source is armed and this control is a
        // registry target, a tap connects it. Guard eventComponent==this so the inner-slider
        // listener path (which also lands here) doesn't double-fire — the ModSlider handles taps
        // that land on the slider itself.
        if (e.eventComponent == this && beginLinkGesture (e)) return;
        if (e.mods.isPopupMenu())          { showLearnMenu(); return; }
        // #139: tapping a control that is already armed for learn CANCELS it (escape hatch for
        // an accidental long-press that left the amber highlight stuck on).
        if (learn.isLearningParam (paramID)) { learn.armLearn (juce::String()); stopTimer(); repaint(); return; }
        pressStart = juce::Time::getMillisecondCounter();
        longPressArmed = true;
        startTimer (60);                   // poll for the long-press threshold
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.eventComponent == this && linkDepthActive) { dragLinkDepth (e); return; }
        if (e.getDistanceFromDragStart() > 8) longPressArmed = false;   // it's a value drag
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.eventComponent == this && linkDepthActive) { endLinkGesture(); return; }
        // #148: a quick tap (released before the long-press fired) on the armed LFO DEST cancels
        // arm mode — the "short-press ON cancels" escape hatch.
        if (longPressArmed && ! e.mouseWasDraggedSinceMouseDown()
            && onShortPressOverride && onShortPressOverride()) { longPressArmed = false; stopTimer(); repaint(); return; }
        longPressArmed = false;
    }

    // ---- Mod-matrix TARGET support (the SINGLE shared path — #56 follow-up). Wired centrally
    // from the destination registry (see the editor), never per control. Any control whose
    // parameter is in the registry gets the connect-ring, whole-cell tap-to-connect + 2 s
    // depth-drag, and a live animation indicator; everything else gets none of it. ----
    void setModTarget (ModLinkController& c, int dest, std::function<float()> animFn)
    { modLink = &c; modDest = dest; modAnimFn = std::move (animFn); modTargetAttached(); }
    bool isModTarget()   const { return modLink != nullptr && modDest > 0; }
    bool isLinkArmable() const { return isModTarget() && modLink->linkArmed(); }
    float modAnim()      const { return modAnimFn ? modAnimFn() : 0.0f; }   // normalized offset for the indicator
    // Per-LFO colour + armed context for the mod-anim indicator (#148). lfoIdx: which LFO (0..2)
    // colours this dest, or -1. armedLfo: the LFO armed for link, or -1 (arm-mode "editable picture").
    int   modLfoIdx()    const { return modLink ? modLink->lfoDrivingDest (modDest) : -1; }
    int   modArmedLfo()  const { return modLink ? modLink->armedLfo() : -1; }
    juce::Colour modColour() const
    { const int i = modLfoIdx(); return i >= 0 ? VASynthLookAndFeel::lfoColour (i) : VASynthLookAndFeel::accentWarm(); }
    bool  inLinkDrag()   const { return linkDepthActive; }
    // #148 LFO Link: the LFO DEST selector overrides its long-press (arm/commit) and a quick tap
    // while armed (cancel) — instead of MIDI-learn / value-cycle. Each returns true if it handled it.
    std::function<bool()> onLongPressOverride, onShortPressOverride;

    bool beginLinkGesture (const juce::MouseEvent& e)
    {
        if (modLink == nullptr || modDest <= 0) return false;
        const juce::uint32 now = juce::Time::getMillisecondCounter();
        if (modLink->linkArmed())
        {
            const int slot = modLink->completeModLink (modDest);
            if (slot < 0) return false;                          // matrix full -> behave as a normal control
            linkSlot = slot; linkDepthActive = true; linkDownY = e.getPosition().y;
            linkDownDepth = modLink->modRouteDepth (slot); linkTime = now; repaint(); return true;
        }
        if (linkSlot >= 0 && now - linkTime < 2000)              // re-grab the just-made route within ~2 s
        { linkDepthActive = true; linkDownY = e.getPosition().y; linkDownDepth = modLink->modRouteDepth (linkSlot); return true; }
        return false;
    }
    void dragLinkDepth (const juce::MouseEvent& e)
    {
        const float d = juce::jlimit (-1.0f, 1.0f, linkDownDepth + (float) (linkDownY - e.getPosition().y) * 0.007f);
        modLink->setModRouteDepth (linkSlot, d); repaint();
    }
    void endLinkGesture() { linkDepthActive = false; linkTime = juce::Time::getMillisecondCounter(); }

    // #148 Inc 3 — slide-to-bounds. In sticky LFO-Link mode a press+drag on a CONTINUOUS target
    // sweeps the knob to define its modulation bounds [lo,hi]; release parks the value at the
    // midpoint and creates the route at the signed half-range depth. A pure tap = full-scale route.
    bool armedForBounds() const { return modLink != nullptr && modDest > 0 && modLink->armedLfo() >= 0; }
    bool beginBoundsCapture (float norm, bool continuous)
    {
        if (! armedForBounds() || ! continuous) return false;
        boundsCapturing = true; boundsStart = boundsMin = boundsMax = juce::jlimit (0.0f, 1.0f, norm); return true;
    }
    bool inBoundsCapture() const { return boundsCapturing; }
    void updateBounds (float norm)
    { norm = juce::jlimit (0.0f, 1.0f, norm); boundsMin = std::min (boundsMin, norm); boundsMax = std::max (boundsMax, norm); repaint(); }
    // Ends the capture. Returns the midpoint (0..1) to park the knob at, or -1 for a tap (full-scale).
    float endBoundsCapture (bool dragged, float releaseNorm)
    {
        boundsCapturing = false; repaint();
        if (! dragged || boundsMax - boundsMin < 0.01f) { if (modLink) modLink->completeModLink (modDest); return -1.0f; }
        const float mid = 0.5f * (boundsMin + boundsMax), half = 0.5f * (boundsMax - boundsMin);
        const float sign = (juce::jlimit (0.0f, 1.0f, releaseNorm) >= boundsStart) ? 1.0f : -1.0f;
        if (modLink) modLink->setModLinkBounds (modDest, sign * half);
        return mid;
    }
    float boundsLo() const { return boundsMin; }
    float boundsHi() const { return boundsMax; }

    // Cyan connect-ring — every armable target draws it (call from paint()).
    void paintModRing (juce::Graphics& g)
    {
        if (! isLinkArmable()) return;
        g.setColour (juce::Colour (0xff4bb3c4).withAlpha (0.9f));
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.5f), 5.0f, 2.0f);
    }

    // Owner-supplied extra right-click/long-press items (e.g. a macro's Restore-default / Rename).
    void addContextMenuItem (juce::String label, std::function<void()> action)
    { extraMenuItems.emplace_back (std::move (label), std::move (action)); }

    // Subclasses call this in paint() to draw the armed outline + CC badge.
    void paintLearnDecorations (juce::Graphics& g)
    {
        const int cc = learn.getCCForParam (paramID);
        if (cc >= 0)
        {
            // A compact CC chip. Placed top-right by default; for controls whose name needs the
            // full top (two-line macros) it drops to the bottom-right so it never collides.
            const int bw = juce::jmin (getWidth() - 2, ccBadgeAtBottom ? 30 : 34);
            auto strip = ccBadgeAtBottom ? getLocalBounds().removeFromBottom (12) : getLocalBounds().removeFromTop (12);
            auto badge = strip.removeFromRight (bw).toFloat().reduced (0.5f);
            g.setColour (VASynthLookAndFeel::accent().withAlpha (0.92f));
            g.fillRoundedRectangle (badge, 2.5f);
            g.setColour (juce::Colours::black);
            g.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::bold)));
            g.drawText ("CC" + juce::String (cc), badge, juce::Justification::centred, false);
        }
        if (learn.isLearningParam (paramID))
        {
            const float a = 0.35f + 0.45f * (float) std::abs (std::sin (juce::Time::getMillisecondCounter() * 0.006));
            g.setColour (VASynthLookAndFeel::accentWarm().withAlpha (a));
            g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.5f), 5.0f, 2.5f);
        }
    }

    const juce::String& parameterID() const { return paramID; }

    // Double-click numeric entry (R3 Group 4): a transient TextEditor over the control,
    // parsed via the parameter's own text<->value conversion. QWERTY note input is
    // auto-suppressed while a TextEditor holds focus (the editor watchdog) and reclaimed
    // when it closes. No-op unless a subclass called enableNumericEntry().
    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        if (numParam == nullptr || numEditor != nullptr) return;
        longPressArmed = false;
        numEditor = std::make_unique<juce::TextEditor>();
        numEditor->setJustification (juce::Justification::centred);
        numEditor->setText (numParam->getCurrentValueAsText(), false);
        numEditor->setBounds (getLocalBounds().withSizeKeepingCentre (juce::jmin (getWidth(), 76), 20));
        numEditor->onReturnKey = [this] { commitNumericEntry(); };
        numEditor->onEscapeKey = [this] { closeNumericEntry(); };
        numEditor->onFocusLost = [this] { commitNumericEntry(); };
        addAndMakeVisible (*numEditor);
        numEditor->selectAll();
        numEditor->grabKeyboardFocus();
    }

protected:
    void enableNumericEntry (juce::RangedAudioParameter* p) { numParam = p; }
    virtual void modTargetAttached() {}   // subclass creates its animation indicator (knob arc / fader ghost)
public:
    // True once this control has actually built its motion indicator. A mod TARGET (isModTarget)
    // whose hasModIndicator() is false is wired-but-dead (data flows, nothing draws — the #12 NOISE
    // bug). Tests assert every target has one, catching the missing-override class of defect.
    virtual bool hasModIndicator() const { return false; }
protected:
    bool ccBadgeAtBottom = false;         // move the CC chip to the bottom (two-line-name controls)

    // Mod-target state (shared by every registry control). Protected so ModSlider + subclass
    // indicators can read what they need through the public accessors above.
    ModLinkController* modLink = nullptr;
    int   modDest = 0, linkSlot = -1, linkDownY = 0;
    bool  linkDepthActive = false;
    float linkDownDepth = 0.0f;
    juce::uint32 linkTime = 0;
    bool  boundsCapturing = false;                    // #148 Inc 3: slide-to-bounds capture in progress
    float boundsStart = 0.0f, boundsMin = 0.0f, boundsMax = 0.0f;
    juce::uint32 armStart = 0;          // #139: when learn was armed, for the stuck-highlight auto-timeout
    std::function<float()> modAnimFn;
    std::vector<std::pair<juce::String, std::function<void()>>> extraMenuItems;

private:
    void commitNumericEntry()
    {
        if (numEditor == nullptr) return;
        if (numParam != nullptr)
            numParam->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, numParam->getValueForText (numEditor->getText())));
        closeNumericEntry();
    }
    void closeNumericEntry()
    {
        if (numEditor == nullptr) return;
        numEditor->onReturnKey = nullptr; numEditor->onEscapeKey = nullptr; numEditor->onFocusLost = nullptr;
        std::shared_ptr<juce::TextEditor> dead (numEditor.release());   // defer the delete so we
        dead->setVisible (false);                                       // never free it inside its
        juce::MessageManager::callAsync ([dead] {});                    // own callback (UAF-safe)
        repaint();
    }

    void timerCallback() override
    {
        if (longPressArmed && juce::Time::getMillisecondCounter() - pressStart > 500)
        {
            longPressArmed = false;
            if (onLongPressOverride && onLongPressOverride()) { stopTimer(); repaint(); return; }   // #148: LFO dest arms link, not learn
            armLearn();
        }
        if (learn.isLearningParam (paramID))
        {
            // #139: an accidental stationary long-press arms learn (amber pulse); it would
            // otherwise stay armed forever if no CC ever arrives, leaving the highlight stuck.
            // Auto-disarm after a grace period so it always clears itself.
            if (juce::Time::getMillisecondCounter() - armStart > kLearnArmTimeoutMs)
            { learn.armLearn (juce::String()); stopTimer(); repaint(); return; }
            repaint();                     // pulse while armed
        }
        else if (! longPressArmed) stopTimer();
    }

    static constexpr juce::uint32 kLearnArmTimeoutMs = 8000;   // #139: auto-clear a stuck learn-arm

    void armLearn()
    {
        learn.armLearn (paramID);
        armStart = juce::Time::getMillisecondCounter();
        startTimer (33);                   // ~30 Hz pulse while armed
        repaint();
    }

    void showLearnMenu()
    {
        juce::PopupMenu m;
        m.addItem (1, "MIDI-learn this control");
        const int cc = learn.getCCForParam (paramID);
        if (cc >= 0) m.addItem (2, "Clear mapping (CC" + juce::String (cc) + ")");
        if (! extraMenuItems.empty())
        {
            m.addSeparator();
            int id = 100;
            for (auto& it : extraMenuItems) m.addItem (id++, it.first);
        }
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                         [this] (int r)
                         {
                             if (r == 1) armLearn();
                             else if (r == 2) { learn.clearParam (paramID); repaint(); }
                             else if (r >= 100 && r - 100 < (int) extraMenuItems.size()) extraMenuItems[(std::size_t) (r - 100)].second();
                         });
    }

    MidiLearnManager& learn;
    juce::String paramID;
    juce::uint32 pressStart = 0;
    bool longPressArmed = false;
    juce::RangedAudioParameter* numParam = nullptr;   // double-click numeric entry target
    std::unique_ptr<juce::TextEditor> numEditor;
};

// A slider that defers to its owning LearnableComponent for the LINK gesture: a tap on the
// slider connects the armed source (and drives the 2 s depth-drag) instead of moving the value.
// Shared by RotaryKnob and LabelledFader so BOTH are mod targets through the one path.
struct ModSlider : juce::Slider
{
    explicit ModSlider (LearnableComponent& o) : owner (o) {}
    bool continuousRange() const { return getNormalisableRange().interval <= 0.0; }   // stepped -> tap-only (no bounds drag)
    void mouseDown (const juce::MouseEvent& e) override
    {
        // #148 Inc 3: sticky LFO-Link mode -> drag the knob to set its modulation bounds (continuous only).
        if (owner.beginBoundsCapture ((float) valueToProportionOfLength (getValue()), continuousRange()))
        { juce::Slider::mouseDown (e); return; }                                       // let the knob follow the pointer
        if (! owner.beginLinkGesture (e)) juce::Slider::mouseDown (e);
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (owner.inBoundsCapture()) { juce::Slider::mouseDrag (e); owner.updateBounds ((float) valueToProportionOfLength (getValue())); return; }
        if (owner.inLinkDrag()) owner.dragLinkDepth (e); else juce::Slider::mouseDrag (e);
    }
    void mouseUp   (const juce::MouseEvent& e) override
    {
        if (owner.inBoundsCapture())
        {
            juce::Slider::mouseUp (e);
            const float mid = owner.endBoundsCapture (e.mouseWasDraggedSinceMouseDown(), (float) valueToProportionOfLength (getValue()));
            if (mid >= 0.0f) setValue (proportionOfLengthToValue (mid), juce::sendNotificationSync);   // park at the midpoint
            return;
        }
        if (owner.inLinkDrag()) owner.endLinkGesture(); else juce::Slider::mouseUp (e);
    }
    LearnableComponent& owner;
};

// ---------------------------------------------------------------------------
// Hardware-style on/off kill switch bound to a bool parameter (lit = on).
class PowerToggle : public juce::Component
{
public:
    PowerToggle (juce::AudioProcessorValueTreeState& apvts, const juce::String& pid, juce::String label)
        : name (std::move (label)), paramID (pid)
    {
        btn.setClickingTogglesState (true);
        btn.setWantsKeyboardFocus (false);
        btn.setButtonText (name);
        btn.setColour (juce::TextButton::buttonColourId,   VASynthLookAndFeel::track());
        btn.setColour (juce::TextButton::buttonOnColourId, VASynthLookAndFeel::accent());
        btn.setColour (juce::TextButton::textColourOnId,   juce::Colours::black);
        btn.setColour (juce::TextButton::textColourOffId,  VASynthLookAndFeel::dim());
        addAndMakeVisible (btn);
        attachment = std::make_unique<juce::ButtonParameterAttachment> (*apvts.getParameter (pid), btn);
        if (auto* p = apvts.getParameter (pid)) btn.setTooltip (p->getName (128));   // hover -> full name
        getProperties().set ("layoutFlex", 0.55);
    }

    void setHelp (const juce::String& text) { btn.setTooltip (text); }   // custom hover help
    const juce::String& parameterID() const { return paramID; }          // for the section guide's marker lookup
    void resized() override { btn.setBounds (getLocalBounds().reduced (2)); }

private:
    juce::String name;
    juce::String paramID;
    juce::TextButton btn;
    std::unique_ptr<juce::ButtonParameterAttachment> attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PowerToggle)
};

// ---------------------------------------------------------------------------
// Vertical fader + name + live value readout, bound to a float parameter.
class LabelledFader : public LearnableComponent
{
public:
    // emphasis = a prominent fader (e.g. MASTER): larger, higher-contrast label,
    // an accent frame, and a bright value readout so it stands out in its section.
    LabelledFader (juce::AudioProcessorValueTreeState& apvts, const juce::String& pid,
                   juce::String displayName, MidiLearnManager& learnMgr, bool emphasise = false)
        : LearnableComponent (learnMgr, pid), name (std::move (displayName)), emphasis (emphasise)
    {
        slider.setSliderStyle (juce::Slider::LinearVertical);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setVelocityBasedMode (false);           // absolute drag distance, not velocity
        slider.setSliderSnapsToMousePosition (false);  // R2 GRAB mode: first touch acquires the
                                                       // control with NO value change; the value
                                                       // moves only on drag, relative to the grab
                                                       // point (snapping caused live-perf surprises).
        slider.setMouseDragSensitivity (kDragPixelsForFullRange);   // R2: gentler drag-to-value
        slider.setWantsKeyboardFocus (false);
        addAndMakeVisible (slider);
        attachment = std::make_unique<juce::SliderParameterAttachment> (*apvts.getParameter (pid), slider);

        param = apvts.getParameter (pid);
        enableNumericEntry (param);
        listenForLearnGestures (slider);
        setTooltipFromParam (apvts, &slider);      // hover -> full parameter name
    }

    // Custom hover help (overrides the param name); routes to the inner slider (the leaf).
    void setHelp (const juce::String& text) { LearnableComponent::setHelp (text, &slider); }

    // Transient readout: the value shows ONLY while the fader is being dragged (nothing at rest).
    // Used where several narrow faders share a row (the ADSR envelope) so the values don't crush.
    void setTransientReadout (bool on)
    {
        transientReadout = on;
        slider.onDragStart = [this] { dragging = true;  repaint(); };
        slider.onDragEnd   = [this] { dragging = false; repaint(); };
    }

    void paint (juce::Graphics& g) override
    {
        if (emphasis)
        {
            // Distinct rounded frame so MASTER reads as the section's headline.
            auto r = getLocalBounds().toFloat().reduced (1.5f);
            g.setColour (VASynthLookAndFeel::accentWarm().withAlpha (0.10f));
            g.fillRoundedRectangle (r, 6.0f);
            g.setColour (VASynthLookAndFeel::accentWarm().withAlpha (0.75f));
            g.drawRoundedRectangle (r, 6.0f, 1.6f);
        }

        // Name: high-contrast (near-white / amber for master), larger for arm's-
        // length reading. Bold so it stays legible on a busy dark panel.
        g.setColour (emphasis ? VASynthLookAndFeel::accentWarm() : VASynthLookAndFeel::ink());
        g.setFont (juce::Font (juce::FontOptions (emphasis ? 15.0f : 13.0f, juce::Font::bold)));
        g.drawFittedText (emphasis ? name.toUpperCase() : name,
                          getLocalBounds().removeFromTop (labelH()), juce::Justification::centred, 1);

        // Live value readout with the parameter's own units/text (auto-fit width), in the accent
        // colour. When transient, it appears only while dragging (nothing at rest); time params
        // read in ms below 1 s, s above.
        if (! transientReadout || dragging)
        {
            g.setColour (emphasis ? VASynthLookAndFeel::ink() : VASynthLookAndFeel::accent());
            g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                                      emphasis ? 14.0f : 12.5f, juce::Font::bold)));
            g.drawFittedText (formatFaderValue(), getLocalBounds().removeFromBottom (labelH()), juce::Justification::centred, 1);
        }

        paintLearnDecorations (g);
        paintModRing (g);
    }

    void resized() override
    {
        const int pad = emphasis ? 6 : 2;
        slider.setBounds (getLocalBounds().reduced (pad, 0).withTrimmedTop (labelH() + 2).withTrimmedBottom (labelH() + 2));
        if (ghost) { ghost->setBounds (slider.getBounds()); ghost->toFront (false); }
    }

    bool hasModIndicator() const override { return ghost != nullptr; }

protected:
    void modTargetAttached() override
    {
        if (ghost == nullptr) { ghost = std::make_unique<FaderModOverlay> (slider, *this); addAndMakeVisible (*ghost); }
        ghost->begin();
        resized();
    }

private:
    int labelH() const { return emphasis ? 20 : 18; }

    // Time params (label "s") read as ms below 1 s, s above; everything else uses the param text.
    juce::String formatFaderValue() const
    {
        if (param != nullptr && param->getLabel() == "s")
        {
            const float sec = param->convertFrom0to1 (param->getValue());
            return sec < 1.0f ? juce::String (juce::roundToInt (sec * 1000.0f)) + " ms"
                              : juce::String (sec, 2) + " s";
        }
        return formatParamValue (param);
    }

    bool transientReadout = false, dragging = false;

    // Mouse-transparent ghost thumb at the modulated position (same colour language as the knob
    // arc), distinct from the base thumb. Repaints ~30 Hz only while the route is actually moving.
    struct FaderModOverlay : juce::Component, private juce::Timer
    {
        FaderModOverlay (juce::Slider& sl, LearnableComponent& o) : s (sl), owner (o) { setInterceptsMouseClicks (false, false); }
        void begin() { startTimerHz (modanim::kTimerHz); }
        void timerCallback() override
        {
            const int armed = owner.modArmedLfo();
            const bool moved = st.tick (owner.modAnim(), juce::Time::getMillisecondCounter());
            if (moved || armed != lastArmed) { lastArmed = armed; repaint(); }
        }
        void paint (juce::Graphics& g) override
        {
            const int armedLfo = owner.modArmedLfo(), lfoIdx = owner.modLfoIdx();
            const juce::Colour col = owner.modColour();
            auto b = getLocalBounds().toFloat();
            // #148 armed "editable picture": a static colour marker at the value marks membership.
            if (armedLfo >= 0 && lfoIdx >= 0)
            {
                const float y = b.getBottom() - (float) s.valueToProportionOfLength (s.getValue()) * b.getHeight();
                const bool mine = lfoIdx == armedLfo;
                g.setColour (VASynthLookAndFeel::lfoColour (lfoIdx).withAlpha (mine ? 0.95f : 0.28f));
                g.fillRoundedRectangle (b.getX() - 3.0f, y - (mine ? 2.5f : 1.5f), b.getWidth() + 6.0f, mine ? 5.0f : 3.0f, 2.0f);
            }
            if (! st.visible()) return;                                 // motion-gated
            float a = st.alpha();
            if (armedLfo >= 0 && lfoIdx != armedLfo) a *= 0.22f;
            const float base = (float) s.valueToProportionOfLength (s.getValue());
            const float curP = juce::jlimit (0.0f, 1.0f, base + st.cur);
            const float lagP = juce::jlimit (0.0f, 1.0f, base + st.lag);
            const float yCur = b.getBottom() - curP * b.getHeight();    // vertical fader: value up
            const float yLag = b.getBottom() - lagP * b.getHeight();
            g.setColour (col.withAlpha (0.35f * a));
            g.fillRect (b.getX() - 1.0f, juce::jmin (yCur, yLag), b.getWidth() + 2.0f, std::abs (yCur - yLag));
            g.setColour (col.withAlpha (0.9f * a));
            g.fillRoundedRectangle (b.getX() - 3.0f, yCur - 2.0f, b.getWidth() + 6.0f, 4.0f, 2.0f);
        }
        juce::Slider& s; LearnableComponent& owner; modanim::State st; int lastArmed = -1;
    };

    juce::String name;
    bool emphasis = false;
    ModSlider slider { *this };
    juce::RangedAudioParameter* param = nullptr;
    std::unique_ptr<juce::SliderParameterAttachment> attachment;
    std::unique_ptr<FaderModOverlay> ghost;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LabelledFader)
};

// ---------------------------------------------------------------------------
// Horizontal fill-bar control: dragging/tapping left->right raises the value, and the FILLED BAR
// itself is the level indicator. Learnable + a registry mod-target + numeric-entry like the knobs
// and faders. Used for the NOISE source level.
class HBarControl : public LearnableComponent
{
public:
    HBarControl (juce::AudioProcessorValueTreeState& apvts, const juce::String& pid,
                 juce::String displayName, MidiLearnManager& learnMgr)
        : LearnableComponent (learnMgr, pid), name (std::move (displayName))
    {
        slider.setSliderStyle (juce::Slider::LinearBar);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setSliderSnapsToMousePosition (true);          // a fill bar: tap-to-position feels natural
        slider.setColour (juce::Slider::trackColourId,      VASynthLookAndFeel::accent());   // the fill
        slider.setColour (juce::Slider::backgroundColourId, VASynthLookAndFeel::track());    // the trough
        slider.setWantsKeyboardFocus (false);
        addAndMakeVisible (slider);
        attachment = std::make_unique<juce::SliderParameterAttachment> (*apvts.getParameter (pid), slider);
        param = apvts.getParameter (pid);
        enableNumericEntry (param);
        listenForLearnGestures (slider);
        setTooltipFromParam (apvts, &slider);
    }

    void setHelp (const juce::String& text) { LearnableComponent::setHelp (text, &slider); }

    void paint (juce::Graphics& g) override
    {
        // The LinearBar slider paints the fill; overlay the value on the right for a readout.
        g.setColour (VASynthLookAndFeel::ink());
        g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 11.0f, juce::Font::bold)));
        g.drawText (formatParamValue (param), getLocalBounds().reduced (7, 0), juce::Justification::centredRight, false);
        paintLearnDecorations (g);
        paintModRing (g);
    }

    void resized() override
    {
        slider.setBounds (getLocalBounds());
        if (ghost) { ghost->setBounds (slider.getBounds()); ghost->toFront (false); }
    }

    bool hasModIndicator() const override { return ghost != nullptr; }

protected:
    // #12: a modulated NOISE level must animate like the knobs/faders. Without this override the
    // base no-op ran and no indicator was ever created (the control was wired but drew nothing).
    void modTargetAttached() override
    {
        if (ghost == nullptr) { ghost = std::make_unique<HBarModOverlay> (slider, *this); addAndMakeVisible (*ghost); }
        ghost->begin();
        resized();
    }

private:
    // Mouse-transparent ghost marker at the modulated position (horizontal: value runs left->right).
    // Motion-gated + repaints ~30 Hz only while the route is actually moving (mirrors FaderModOverlay).
    struct HBarModOverlay : juce::Component, private juce::Timer
    {
        HBarModOverlay (juce::Slider& sl, LearnableComponent& o) : s (sl), owner (o) { setInterceptsMouseClicks (false, false); }
        void begin() { startTimerHz (modanim::kTimerHz); }
        void timerCallback() override { if (st.tick (owner.modAnim(), juce::Time::getMillisecondCounter())) repaint(); }
        void paint (juce::Graphics& g) override
        {
            if (! st.visible()) return;                                 // motion-gated: nothing at rest
            const float a = st.alpha();
            const float base = (float) s.valueToProportionOfLength (s.getValue());
            const float curP = juce::jlimit (0.0f, 1.0f, base + st.cur);
            const float lagP = juce::jlimit (0.0f, 1.0f, base + st.lag);
            auto b = getLocalBounds().toFloat();
            const float xCur = b.getX() + curP * b.getWidth();          // horizontal bar: value right
            const float xLag = b.getX() + lagP * b.getWidth();
            g.setColour (VASynthLookAndFeel::accentWarm().withAlpha (0.35f * a));   // trail echo -> current
            g.fillRect (juce::jmin (xCur, xLag), b.getY() - 1.0f, std::abs (xCur - xLag), b.getHeight() + 2.0f);
            g.setColour (VASynthLookAndFeel::accentWarm().withAlpha (0.9f * a));    // ghost marker at current
            g.fillRoundedRectangle (xCur - 2.0f, b.getY() - 3.0f, 4.0f, b.getHeight() + 6.0f, 2.0f);
        }
        juce::Slider& s; LearnableComponent& owner; modanim::State st;
    };

    juce::String name;
    ModSlider slider { *this };
    juce::RangedAudioParameter* param = nullptr;
    std::unique_ptr<juce::SliderParameterAttachment> attachment;
    std::unique_ptr<HBarModOverlay> ghost;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HBarControl)
};

// ---------------------------------------------------------------------------
// VERTICAL fill-bar control — the upright twin of HBarControl, for an axis that reads
// bottom-to-top. Same contract: learnable, a registry mod target with its own motion
// indicator, numeric entry on double-click. Used for the NOISE field's FOCUS axis, which
// needs to be a LINK/learn target in its own right (you must be able to route an LFO at
// focus alone) while the pad beside it drives both axes at once.
class VBarControl : public LearnableComponent
{
public:
    VBarControl (juce::AudioProcessorValueTreeState& apvts, const juce::String& pid,
                 juce::String displayName, MidiLearnManager& learnMgr)
        : LearnableComponent (learnMgr, pid), name (std::move (displayName))
    {
        slider.setSliderStyle (juce::Slider::LinearBarVertical);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setSliderSnapsToMousePosition (true);
        slider.setColour (juce::Slider::trackColourId,      VASynthLookAndFeel::accent());
        slider.setColour (juce::Slider::backgroundColourId, VASynthLookAndFeel::track());
        slider.setWantsKeyboardFocus (false);
        addAndMakeVisible (slider);
        attachment = std::make_unique<juce::SliderParameterAttachment> (*apvts.getParameter (pid), slider);
        param = apvts.getParameter (pid);
        enableNumericEntry (param);
        listenForLearnGestures (slider);
        setTooltipFromParam (apvts, &slider);
    }

    void setHelp (const juce::String& text) { LearnableComponent::setHelp (text, &slider); }

    void paint (juce::Graphics& g) override
    {
        // The bar paints its own fill; the name rides the bottom edge so the column reads
        // at a glance without a separate label row eating the (already short) height.
        g.setColour (VASynthLookAndFeel::ink().withAlpha (0.85f));
        g.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::bold)));
        g.drawText (name, getLocalBounds().removeFromBottom (11), juce::Justification::centred, false);
        paintLearnDecorations (g);
        paintModRing (g);
    }

    void resized() override
    {
        slider.setBounds (getLocalBounds().withTrimmedBottom (12));
        if (ghost) { ghost->setBounds (slider.getBounds()); ghost->toFront (false); }
    }

    bool hasModIndicator() const override { return ghost != nullptr; }

protected:
    void modTargetAttached() override
    {
        if (ghost == nullptr) { ghost = std::make_unique<VBarModOverlay> (slider, *this); addAndMakeVisible (*ghost); }
        ghost->begin();
        resized();
    }

private:
    // Mouse-transparent ghost marker at the modulated position (vertical: value runs upward).
    struct VBarModOverlay : juce::Component, private juce::Timer
    {
        VBarModOverlay (juce::Slider& sl, LearnableComponent& o) : s (sl), owner (o) { setInterceptsMouseClicks (false, false); }
        void begin() { startTimerHz (modanim::kTimerHz); }
        void timerCallback() override { if (st.tick (owner.modAnim(), juce::Time::getMillisecondCounter())) repaint(); }
        void paint (juce::Graphics& g) override
        {
            if (! st.visible()) return;                                 // motion-gated: nothing at rest
            const float a = st.alpha();
            const float base = (float) s.valueToProportionOfLength (s.getValue());
            const float curP = juce::jlimit (0.0f, 1.0f, base + st.cur);
            const float lagP = juce::jlimit (0.0f, 1.0f, base + st.lag);
            auto b = getLocalBounds().toFloat();
            const float yCur = b.getBottom() - curP * b.getHeight();
            const float yLag = b.getBottom() - lagP * b.getHeight();
            g.setColour (owner.modColour().withAlpha (0.35f * a));      // trail echo -> current
            g.fillRect (b.getX() - 1.0f, juce::jmin (yCur, yLag), b.getWidth() + 2.0f, std::abs (yCur - yLag));
            g.setColour (owner.modColour().withAlpha (0.9f * a));       // ghost marker at current
            g.fillRoundedRectangle (b.getX() - 2.0f, yCur - 2.0f, b.getWidth() + 4.0f, 4.0f, 2.0f);
        }
        juce::Slider& s; LearnableComponent& owner; modanim::State st;
    };

    juce::String name;
    ModSlider slider { *this };
    juce::RangedAudioParameter* param = nullptr;
    std::unique_ptr<juce::SliderParameterAttachment> attachment;
    std::unique_ptr<VBarModOverlay> ghost;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VBarControl)
};

// ---------------------------------------------------------------------------
// NOISE XY — a 2D touch surface over the noise source's shaping field. One drag sets
// BOTH coordinates, which is the whole point: the field is a continuous surface, not a
// pair of knobs that happen to sit next to each other.
//
//   bottom edge   the classic noise colours, left to right: BROWN PINK WHITE BRIGHT
//   rising        the noise narrows into a focused band around the x position
//   top           a tight, pitched band -- noise with a note in it
//
// The pad is the registry mod target for the X axis (so LINK, the LFO-Link bounds drag
// and the motion indicator all work on it through the ONE shared path in the editor,
// with no per-control wiring). The Y axis has its own slim FOCUS rail beside the pad,
// for the same reasons -- a 2D surface cannot answer "which axis did you mean?" when a
// LINK tap lands on it, and guessing would be worse than an honest second target.
//
// Double-click (or the context menu) returns the field to WHITE -- the exact bypass
// point, where the shaping filter leaves the signal path entirely.
class NoiseXYPad : public LearnableComponent
{
public:
    NoiseXYPad (juce::AudioProcessorValueTreeState& apvts, const juce::String& xId,
                const juce::String& yId, MidiLearnManager& learnMgr)
        : LearnableComponent (learnMgr, xId)
    {
        xParam = apvts.getParameter (xId);
        yParam = apvts.getParameter (yId);
        // Repaint when either axis moves from ANYWHERE (a preset load, a DAW automation
        // lane, a mapped CC, the FOCUS rail beside us) -- the indicator and the readout
        // must show the live value, never a stale echo of the last drag.
        xAtt = std::make_unique<juce::ParameterAttachment> (*xParam, [this] (float) { repaint(); });
        yAtt = std::make_unique<juce::ParameterAttachment> (*yParam, [this] (float) { repaint(); });
        addContextMenuItem ("Reset noise to WHITE (bypass)", [this] { resetToBypass(); });
        setHelp ("Noise character: drag across for colour (brown/pink/white/bright), up to focus it "
                 "into a band; double-click resets to white");
    }

    // ---- gestures ----------------------------------------------------------
    // Priority order mirrors ModSlider exactly, so the pad behaves like every other
    // target: LFO-Link bounds drag, then LINK connect, then MIDI-learn, then the value.
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (beginBoundsCapture (normX (e), true)) { applyMouse (e); return; }
        LearnableComponent::mouseDown (e);          // LINK connect / context menu / long-press arm
        if (inLinkDrag() || e.mods.isPopupMenu()) return;
        beginGestures();
        dragging = true;
        applyMouse (e);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (inBoundsCapture()) { applyMouse (e); updateBounds (normX (e)); return; }
        if (inLinkDrag())      { dragLinkDepth (e); return; }
        LearnableComponent::mouseDrag (e);          // cancels a pending long-press once it is a real drag
        if (dragging) applyMouse (e);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (inBoundsCapture())
        {
            const float mid = endBoundsCapture (e.mouseWasDraggedSinceMouseDown(), normX (e));
            if (mid >= 0.0f) setNorm (*xParam, mid);
            return;
        }
        if (inLinkDrag()) { endLinkGesture(); return; }
        LearnableComponent::mouseUp (e);
        if (dragging) { endGestures(); dragging = false; }
        repaint();
    }

    // The pad has no single number to type, so the double-click slot carries the reset
    // instead of numeric entry (enableNumericEntry is deliberately never called here).
    void mouseDoubleClick (const juce::MouseEvent&) override { resetToBypass(); }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (VASynthLookAndFeel::track());
        g.fillRoundedRectangle (b, 5.0f);

        // The bottom edge carries the colour axis as an actual gradient, so the map is
        // legible before you have read a word of the manual: dark at brown, neutral at
        // white, bright toward the right.
        auto strip = b.withTop (b.getBottom() - 7.0f).reduced (3.0f, 0.0f);
        juce::ColourGradient grad (juce::Colour (0xff4a3626), strip.getX(), 0.0f,
                                   juce::Colour (0xffcfe6ea), strip.getRight(), 0.0f, false);
        grad.addColour (0.5, juce::Colour (0xff9aa3a6));
        g.setGradientFill (grad);
        g.fillRoundedRectangle (strip, 2.0f);

        const float x = xNorm(), y = yNorm();
        const auto pos = pointFor (x, y);

        // Crosshair + puck: PERSISTENT, so the field's position is readable at rest.
        g.setColour (VASynthLookAndFeel::accent().withAlpha (0.28f));
        g.drawLine (b.getX(), pos.y, b.getRight(), pos.y, 1.0f);
        g.drawLine (pos.x, b.getY(), pos.x, b.getBottom(), 1.0f);
        g.setColour (VASynthLookAndFeel::accent());
        g.fillEllipse (pos.x - 4.5f, pos.y - 4.5f, 9.0f, 9.0f);
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.drawEllipse (pos.x - 4.5f, pos.y - 4.5f, 9.0f, 9.0f, 1.0f);

        // Live readout. In the tilt region it names the COLOUR (deliberately a word, not a
        // slope in dB/oct -- this is a character control, not a measurement tool). It swaps to
        // whichever half the puck is NOT in, so the value never hides under your own finger,
        // and rides a translucent chip so it stays legible over the colour axis.
        auto text = b.reduced (5.0f, 0.0f).withHeight (13.0f);
        text = y < 0.5f ? text.withY (b.getY() + 3.0f)                     // puck low  -> read high
                        : text.withY (b.getBottom() - 22.0f);              // puck high -> read low
        g.setColour (VASynthLookAndFeel::track().withAlpha (0.85f));
        g.fillRoundedRectangle (text, 3.0f);
        g.setColour (VASynthLookAndFeel::ink());
        g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 9.5f, juce::Font::bold)));
        g.drawFittedText (readout(), text.reduced (4.0f, 0.0f).toNearestInt(), juce::Justification::centredLeft, 1);

        paintLearnDecorations (g);
        paintModRing (g);
    }

    void resized() override
    {
        if (ghost) { ghost->setBounds (getLocalBounds()); ghost->toFront (false); }
    }

    bool hasModIndicator() const override { return ghost != nullptr; }

    // Exposed for the smoke harness: what the panel is currently telling the user.
    juce::String readoutText() const { return readout(); }

protected:
    void modTargetAttached() override
    {
        if (ghost == nullptr) { ghost = std::make_unique<XYModOverlay> (*this); addAndMakeVisible (*ghost); }
        ghost->begin();
        resized();
    }

private:
    // Mouse-transparent ghost column at the MODULATED x position. The Y axis draws its own
    // on the FOCUS rail beside the pad, so a route to either axis is visible where it acts.
    // Motion-gated like every other indicator: nothing is drawn while the route sits still.
    struct XYModOverlay : juce::Component, private juce::Timer
    {
        explicit XYModOverlay (NoiseXYPad& o) : owner (o) { setInterceptsMouseClicks (false, false); }
        void begin() { startTimerHz (modanim::kTimerHz); }
        void timerCallback() override { if (st.tick (owner.modAnim(), juce::Time::getMillisecondCounter())) repaint(); }
        void paint (juce::Graphics& g) override
        {
            if (! st.visible()) return;
            const float a = st.alpha();
            auto inner = getLocalBounds().toFloat().reduced (6.0f);
            const float base = owner.xNorm();
            const float xCur = inner.getX() + juce::jlimit (0.0f, 1.0f, base + st.cur) * inner.getWidth();
            const float xLag = inner.getX() + juce::jlimit (0.0f, 1.0f, base + st.lag) * inner.getWidth();
            auto b = getLocalBounds().toFloat().reduced (2.0f);
            g.setColour (owner.modColour().withAlpha (0.30f * a));      // trail echo -> current
            g.fillRect (juce::jmin (xCur, xLag), b.getY(), std::abs (xCur - xLag), b.getHeight());
            g.setColour (owner.modColour().withAlpha (0.9f * a));       // ghost column at current
            g.fillRoundedRectangle (xCur - 1.5f, b.getY(), 3.0f, b.getHeight(), 1.5f);
        }
        NoiseXYPad& owner; modanim::State st;
    };

    float xNorm() const { return xParam != nullptr ? xParam->getValue() : NoiseShaper::kDefaultX; }
    float yNorm() const { return yParam != nullptr ? yParam->getValue() : NoiseShaper::kDefaultY; }

    juce::Point<float> pointFor (float x, float y) const
    {
        auto b = getLocalBounds().toFloat().reduced (6.0f);
        return { b.getX() + juce::jlimit (0.0f, 1.0f, x) * b.getWidth(),
                 b.getBottom() - juce::jlimit (0.0f, 1.0f, y) * b.getHeight() };
    }
    float normX (const juce::MouseEvent& e) const
    {
        auto b = getLocalBounds().toFloat().reduced (6.0f);
        return b.getWidth()  <= 0.0f ? 0.5f : juce::jlimit (0.0f, 1.0f, ((float) e.position.x - b.getX()) / b.getWidth());
    }
    float normY (const juce::MouseEvent& e) const
    {
        auto b = getLocalBounds().toFloat().reduced (6.0f);
        return b.getHeight() <= 0.0f ? 0.0f : juce::jlimit (0.0f, 1.0f, (b.getBottom() - (float) e.position.y) / b.getHeight());
    }

    static void setNorm (juce::RangedAudioParameter& p, float v) { p.setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, v)); }
    void applyMouse (const juce::MouseEvent& e)
    {
        if (xParam != nullptr) setNorm (*xParam, normX (e));
        if (yParam != nullptr) setNorm (*yParam, normY (e));
        repaint();
    }
    void beginGestures() { if (xParam) xParam->beginChangeGesture(); if (yParam) yParam->beginChangeGesture(); }
    void endGestures()   { if (xParam) xParam->endChangeGesture();   if (yParam) yParam->endChangeGesture(); }

    void resetToBypass()
    {
        // Exactly the defaults, not "about the middle": the DSP bypass is an equality test,
        // so an approximate reset would leave the filter in the path for no benefit.
        beginGestures();
        if (xParam != nullptr) setNorm (*xParam, NoiseShaper::kDefaultX);
        if (yParam != nullptr) setNorm (*yParam, NoiseShaper::kDefaultY);
        endGestures();
        repaint();
    }

    juce::String readout() const
    {
        const float x = xNorm(), y = yNorm();
        if (y > 0.0f)
            return juce::String (juce::roundToInt (NoiseShaper::focusHz (x))) + " Hz FOCUS "
                 + juce::String (juce::roundToInt (y * 100.0f)) + "%";
        return colourWord (x);
    }
    // Four named zones across the tilt axis, centred on the documented anchors
    // (0.0 brown / 0.25 pink / 0.5 white) with everything clearly rising called BRIGHT.
    static const char* colourWord (float x)
    {
        if (x < 0.125f) return "BROWN";
        if (x < 0.400f) return "PINK";
        if (x < 0.600f) return "WHITE";
        return "BRIGHT";
    }

    juce::RangedAudioParameter* xParam = nullptr;
    juce::RangedAudioParameter* yParam = nullptr;
    std::unique_ptr<juce::ParameterAttachment> xAtt, yAtt;
    std::unique_ptr<XYModOverlay> ghost;
    bool dragging = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NoiseXYPad)
};

// ---------------------------------------------------------------------------
// Transient toast notification (e.g. "Launchkey Mini connected"). Holds for ~2 s
// then fades. Intercepts no input and refuses focus, so it never disturbs the
// controls beneath it or the QWERTY note input.
class Toast : public juce::Component,
              private juce::Timer
{
public:
    Toast()
    {
        setInterceptsMouseClicks (false, false);
        setWantsKeyboardFocus (false);
        setVisible (false);
    }

    void show (const juce::String& message)
    {
        text = message;
        ticks = 0;
        alpha = 1.0f;
        setVisible (true);
        toFront (false);
        startTimerHz (30);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (VASynthLookAndFeel::panelLight().withAlpha (alpha * 0.96f));
        g.fillRoundedRectangle (r, 8.0f);
        g.setColour (VASynthLookAndFeel::accent().withAlpha (alpha));
        g.drawRoundedRectangle (r.reduced (0.75f), 8.0f, 1.4f);
        g.setColour (VASynthLookAndFeel::ink().withAlpha (alpha));
        g.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
        g.drawFittedText (text, getLocalBounds().reduced (14, 0), juce::Justification::centred, 1);
    }

private:
    void timerCallback() override
    {
        ++ticks;
        if (ticks > 60)                                   // ~2 s hold, then ~0.5 s fade
            alpha = juce::jmax (0.0f, 1.0f - (float) (ticks - 60) / 15.0f);
        if (alpha <= 0.0f) { setVisible (false); stopTimer(); }
        repaint();
    }

    juce::String text;
    float alpha = 1.0f;
    int   ticks = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Toast)
};

// ---------------------------------------------------------------------------
// Rotary knob + name + live value, bound to a float parameter. MIDI-learnable and
// focus-refusing like the faders; used in the FX panel where knobs read better
// than a wall of faders.
class RotaryKnob : public LearnableComponent
{
public:
    // sideLabel: knob on the LEFT (square) with the name + value stacked to its
    // right — for wide/short rows (e.g. the filter's vertical knob column) where a
    // name-above/value-below stack would leave the row half-empty.
    RotaryKnob (juce::AudioProcessorValueTreeState& apvts, const juce::String& pid,
                juce::String displayName, MidiLearnManager& learnMgr, bool sideLabelLayout = false)
        : LearnableComponent (learnMgr, pid), name (std::move (displayName)), sideLabel (sideLabelLayout)
    {
        slider.setSliderStyle (juce::Slider::RotaryVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setVelocityBasedMode (false);           // relative drag distance (grab mode)
        slider.setSliderSnapsToMousePosition (false);  // R2: acquire on touch, no value jump
        slider.setMouseDragSensitivity (kDragPixelsForFullRange);   // R2: gentler drag-to-value
        slider.setWantsKeyboardFocus (false);
        addAndMakeVisible (slider);
        attachment = std::make_unique<juce::SliderParameterAttachment> (*apvts.getParameter (pid), slider);

        param = apvts.getParameter (pid);
        enableNumericEntry (param);
        listenForLearnGestures (slider);
        setTooltipFromParam (apvts, &slider);      // hover -> full parameter name
    }

    // Custom hover help (overrides the param name); routes to the inner slider (the leaf).
    void setHelp (const juce::String& text) { LearnableComponent::setHelp (text, &slider); }

    void paint (juce::Graphics& g) override
    {
        const juce::String text = formatParamValue (param);
        if (sideLabel)
        {
            auto lab = getLocalBounds().withTrimmedLeft (getHeight() + 4);
            g.setColour (VASynthLookAndFeel::ink());
            g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
            g.drawFittedText (name, lab.removeFromTop (lab.getHeight() * 3 / 5), juce::Justification::centredLeft, 1);
            g.setColour (VASynthLookAndFeel::accent());
            g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 11.5f, juce::Font::bold)));
            g.drawFittedText (text, lab, juce::Justification::centredLeft, 1);
            paintLearnDecorations (g);
            paintModRing (g);
            return;
        }

        // Name: shrink-to-fit (down to 0.55x) across up to nameLines lines, so a long macro
        // assignment ("Resonance", "Filter Env Amt") reads in full rather than ellipsizing.
        g.setColour (VASynthLookAndFeel::ink());
        g.setFont (juce::Font (juce::FontOptions (nameLines > 1 ? 11.5f : 12.5f, juce::Font::bold)));
        g.drawFittedText (name, getLocalBounds().removeFromTop (nameTopH()),
                          juce::Justification::centredTop, nameLines, 0.55f);

        // Value: always for showValue knobs; for a transient readout, only while dragging (nothing
        // at rest) — restores the macros' readout without the clutter.
        if (showValue || (transientReadout && dragging))
        {
            g.setColour (VASynthLookAndFeel::accent());
            g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::bold)));
            g.drawFittedText (text, getLocalBounds().removeFromBottom (15), juce::Justification::centred, 1);
        }

        paintLearnDecorations (g);
        paintModRing (g);
    }

    void resized() override
    {
        if (sideLabel)
            slider.setBounds (getLocalBounds().removeFromLeft (getHeight()));
        else
            slider.setBounds (getLocalBounds().withTrimmedTop (nameTopH() + 1).withTrimmedBottom (showValue ? 16 : 2));
        if (overlay) { overlay->setBounds (slider.getBounds()); overlay->toFront (false); }
    }

    // Allow the name to wrap to N lines (macros use 2 so full assignment names fit). The CC chip
    // moves to the bottom so it never overlaps the taller name.
    void setNameLines (int n) { nameLines = juce::jmax (1, n); ccBadgeAtBottom = nameLines > 1; resized(); repaint(); }

    // Transient value readout: the value shows only while the knob is being dragged (nothing at rest).
    void setTransientReadout (bool on)
    {
        transientReadout = on;
        slider.onDragStart = [this] { dragging = true;  repaint(); };
        slider.onDragEnd   = [this] { dragging = false; repaint(); };
    }

    // Update the displayed name (e.g. a macro showing its assigned target).
    void setDisplayName (juce::String n) { if (n != name) { name = std::move (n); repaint(); } }

    // Override drag sensitivity for this knob (pixels of travel for the full range;
    // fewer px = more responsive). Default is kDragPixelsForFullRange for all controls.
    void setDragPixels (int px) { slider.setMouseDragSensitivity (juce::jmax (1, px)); }
    int  dragPixels() const { return slider.getMouseDragSensitivity(); }   // for tests / audits

    // Accept BOTH horizontal and vertical drag (default is vertical only). For knobs high in
    // the window (the top-bar macros) this lets a touch drag sideways to adjust instead of
    // being forced upward into the OS title bar, which on a windowed touch screen would grab
    // the drag and move the window. The mouse cursor is pinned during any rotary drag anyway.
    void setBothAxisDrag() { slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag); }

    // Hide the numeric value readout (macros — keeps the top bar compact).
    void setShowValue (bool b) { if (b != showValue) { showValue = b; resized(); repaint(); } }

    bool hasModIndicator() const override { return overlay != nullptr; }

protected:
    // The base wired this control as a mod target: bring up the arc animation reading modAnim().
    void modTargetAttached() override
    {
        if (overlay == nullptr) { overlay = std::make_unique<ModOverlay> (slider); addAndMakeVisible (*overlay); }
        overlay->modFn    = [this] { return modAnim(); };
        overlay->colourFn = [this] { return modColour(); };     // arc tinted by the driving LFO
        overlay->lfoIdxFn = [this] { return modLfoIdx(); };     // for the per-LFO dash (colour-blind cue)
        overlay->armedFn  = [this] { return modArmedLfo(); };   // arm-mode "editable picture"
        overlay->boundsFn = [this] { return inBoundsCapture() ? juce::Point<float> (boundsLo(), boundsHi()) : juce::Point<float> (-1.0f, -1.0f); };
        overlay->begin();
        resized();
    }

private:
    int  nameTopH() const { return nameLines > 1 ? 24 : 16; }
    int  nameLines = 1;
    bool transientReadout = false, dragging = false;

    // Moving tick + faint span showing the live LFO-modulated position on the knob arc.
    // Mouse-transparent (never blocks dragging); geometry mirrors VASynthLookAndFeel's rotary.
    struct ModOverlay : juce::Component, private juce::Timer
    {
        explicit ModOverlay (juce::Slider& sl) : s (sl) { setInterceptsMouseClicks (false, false); }
        void begin() { startTimerHz (modanim::kTimerHz); }
        void timerCallback() override
        {
            const int armed = armedFn ? armedFn() : -1;      // arm-mode changes must repaint even at rest
            const bool moved = modFn && st.tick (modFn(), juce::Time::getMillisecondCounter());
            if (moved || armed != lastArmed) { lastArmed = armed; repaint(); }
        }
        // Per-LFO colour-blind cue: LFO1 solid, LFO2 dashed, LFO3 dotted.
        void strokeCued (juce::Graphics& g, const juce::Path& p, float w, juce::Colour c, int lfoIdx)
        {
            g.setColour (c);
            juce::PathStrokeType st2 (w, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
            if (lfoIdx == 1 || lfoIdx == 2)
            {
                const float dash1[] { w * 2.4f, w * 1.6f };   // LFO2: dashed
                const float dash2[] { w * 0.9f, w * 1.6f };   // LFO3: dotted
                juce::Path dp; st2.createDashedStroke (dp, p, lfoIdx == 1 ? dash1 : dash2, 2);
                g.fillPath (dp);
            }
            else g.strokePath (p, st2);
        }
        void paint (juce::Graphics& g) override
        {
            const int armedLfo = armedFn  ? armedFn()  : -1;   // LFO armed for link (else -1)
            const int lfoIdx   = lfoIdxFn ? lfoIdxFn() : -1;   // LFO colouring this dest (else -1)
            const juce::Colour col = colourFn ? colourFn() : VASynthLookAndFeel::accentWarm();
            auto b = getLocalBounds().toFloat().reduced (3.0f);
            const float radius = juce::jmin (b.getWidth(), b.getHeight()) * 0.5f;
            const float cx = b.getCentreX(), cy = b.getCentreY();
            const float lineW = juce::jmax (2.5f, radius * 0.16f);
            const float arcR = radius - lineW * 0.5f;

            // Arm mode = "editable picture": a STATIC ring marks this dest's link membership.
            if (armedLfo >= 0 && lfoIdx >= 0)
            {
                juce::Path ring; ring.addCentredArc (cx, cy, arcR, arcR, 0.0f, 0.0f, juce::MathConstants<float>::twoPi, true);
                const bool mine = lfoIdx == armedLfo;          // linked to the armed LFO -> bold; other LFO -> faint
                strokeCued (g, ring, mine ? lineW : lineW * 0.7f,
                            VASynthLookAndFeel::lfoColour (lfoIdx).withAlpha (mine ? 0.95f : 0.28f), lfoIdx);
            }

            // Inc 3 live bounds readout: while sliding to set bounds, draw the [lo,hi] arc + the %s,
            // since the knob rests at the midpoint after release (the drag is otherwise invisible).
            const juce::Point<float> bnd = boundsFn ? boundsFn() : juce::Point<float> (-1.0f, -1.0f);
            if (bnd.x >= 0.0f)
            {
                const auto rp2 = s.getRotaryParameters();
                const auto a2 = [&] (float p) { return rp2.startAngleRadians + p * (rp2.endAngleRadians - rp2.startAngleRadians); };
                const juce::Colour bc = (armedLfo >= 0 ? VASynthLookAndFeel::lfoColour (armedLfo) : col);
                juce::Path span; span.addCentredArc (cx, cy, arcR, arcR, 0.0f, a2 (bnd.x), a2 (bnd.y), true);
                g.setColour (bc.withAlpha (0.9f));
                g.strokePath (span, juce::PathStrokeType (lineW * 1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
                g.drawText (juce::String (juce::roundToInt (bnd.x * 100)) + "-" + juce::String (juce::roundToInt (bnd.y * 100)) + "%",
                            getLocalBounds(), juce::Justification::centred, false);
                return;   // during the drag, show only the bounds
            }

            if (! modFn || ! st.visible()) return;             // motion-gated animated arc
            float a = st.alpha();
            if (armedLfo >= 0 && lfoIdx != armedLfo) a *= 0.22f;   // dim non-armed-LFO motion while arming
            const auto rp = s.getRotaryParameters();
            const float base = (float) s.valueToProportionOfLength (s.getValue());
            const float cur  = juce::jlimit (0.0f, 1.0f, base + st.cur);
            const float lag  = juce::jlimit (0.0f, 1.0f, base + st.lag);
            const auto ang = [&] (float p) { return rp.startAngleRadians + p * (rp.endAngleRadians - rp.startAngleRadians); };
            juce::Path trail; trail.addCentredArc (cx, cy, arcR, arcR, 0.0f, ang (lag), ang (cur), true);
            strokeCued (g, trail, lineW, col.withAlpha (0.45f * a), lfoIdx);
            const float a1 = ang (cur);
            const juce::Point<float> pt (cx + std::sin (a1) * arcR, cy - std::cos (a1) * arcR);
            g.setColour (col.withAlpha (a));
            g.fillEllipse (pt.x - 3.0f, pt.y - 3.0f, 6.0f, 6.0f);
        }
        juce::Slider& s;
        std::function<float()> modFn;
        std::function<juce::Colour()> colourFn;
        std::function<int()> lfoIdxFn, armedFn;
        std::function<juce::Point<float>()> boundsFn;   // (lo,hi) while sliding to bounds, else (-1,-1)
        int lastArmed = -1;
        modanim::State st;
    };

    juce::String name;
    bool sideLabel = false;
    bool showValue = true;
    ModSlider slider { *this };                       // the shared LINK-aware slider (see LearnableComponent)

    juce::RangedAudioParameter* param = nullptr;
    std::unique_ptr<juce::SliderParameterAttachment> attachment;
    std::unique_ptr<ModOverlay> overlay;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RotaryKnob)
};

// ---------------------------------------------------------------------------
// Touch-friendly segmented button row for a choice parameter (one tap, visible
// state) — preferred over a dropdown where the option count allows.
class SegmentedControl : public LearnableComponent
{
public:
    SegmentedControl (juce::AudioProcessorValueTreeState& apvts, const juce::String& pid,
                      juce::String displayName, MidiLearnManager& learnMgr)
        : LearnableComponent (learnMgr, pid), name (std::move (displayName))
    {
        choice = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (pid));
        jassert (choice != nullptr);

        // Segmented controls need more width than a single fader so labels read.
        getProperties().set ("layoutFlex", juce::jmax (2.2, 0.85 * choice->choices.size()));

        for (int i = 0; i < choice->choices.size(); ++i)
        {
            auto* b = buttons.add (new juce::TextButton (choice->choices[i]));
            b->setClickingTogglesState (false);
            b->setWantsKeyboardFocus (false);
            b->setColour (juce::TextButton::buttonColourId, VASynthLookAndFeel::track());
            b->setColour (juce::TextButton::buttonOnColourId, VASynthLookAndFeel::accent());
            b->setColour (juce::TextButton::textColourOffId, VASynthLookAndFeel::dim());
            b->setColour (juce::TextButton::textColourOnId, juce::Colours::black);
            const int idx = i;
            b->onClick = [this, idx] { setIndex (idx); };
            b->setTooltip (choice->getName (128));      // hover -> full parameter name
            addAndMakeVisible (b);
            listenForLearnGestures (*b);
        }
        setTooltipFromParam (apvts);
        attachment = std::make_unique<juce::ParameterAttachment> (
            *choice, [this] (float) { refresh(); }, nullptr);
        attachment->sendInitialUpdate();
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (VASynthLookAndFeel::ink());
        g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
        g.drawText (name, getLocalBounds().removeFromTop (18), juce::Justification::centred, false);
        paintLearnDecorations (g);
    }

    void resized() override
    {
        // Stack buttons vertically — full-width finger targets filling the column.
        auto r = getLocalBounds().withTrimmedTop (18);
        const int n = buttons.size();
        if (n == 0) return;
        const int h = r.getHeight() / n;
        for (int i = 0; i < n; ++i)
            buttons[i]->setBounds (r.removeFromTop (i == n - 1 ? r.getHeight() : h).reduced (2, 2));
    }

private:
    void setIndex (int i)
    {
        choice->beginChangeGesture();
        choice->setValueNotifyingHost (choice->convertTo0to1 ((float) i));
        choice->endChangeGesture();
        refresh();
    }
    void refresh()
    {
        const int current = choice->getIndex();
        for (int i = 0; i < buttons.size(); ++i)
            buttons[i]->setToggleState (i == current, juce::dontSendNotification);
    }

    juce::String name;
    juce::AudioParameterChoice* choice = nullptr;
    juce::OwnedArray<juce::TextButton> buttons;
    std::unique_ptr<juce::ParameterAttachment> attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SegmentedControl)
};

// ---------------------------------------------------------------------------
// Horizontal one-tap selector for a choice parameter — headerless (the section
// header names it), buttons spread left-to-right. This is the R2 layout's segmented
// grammar (osc wave, LFO dest, filter type, chord degree/quality). Optional short
// labels override the parameter's long choice names ("Square" -> "SQR"). Optionally
// draws its own tinted labels above/beside (headerless by default). MIDI-learnable
// and focus-refusing like every control.
class HSelector : public LearnableComponent
{
public:
    HSelector (juce::AudioProcessorValueTreeState& apvts, const juce::String& pid,
               MidiLearnManager& learnMgr, juce::StringArray labelOverride = {})
        : LearnableComponent (learnMgr, pid)
    {
        choice = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (pid));
        jassert (choice != nullptr);
        labels = labelOverride.isEmpty() ? choice->choices : labelOverride;

        for (int i = 0; i < choice->choices.size(); ++i)
        {
            auto* b = buttons.add (new juce::TextButton (labels[juce::jmin (i, labels.size() - 1)]));
            b->setClickingTogglesState (false);
            b->setWantsKeyboardFocus (false);
            b->setColour (juce::TextButton::buttonColourId, VASynthLookAndFeel::track());
            b->setColour (juce::TextButton::buttonOnColourId, VASynthLookAndFeel::accent());
            b->setColour (juce::TextButton::textColourOffId, VASynthLookAndFeel::ink());
            b->setColour (juce::TextButton::textColourOnId, juce::Colours::black);
            const int idx = i;
            // Tapping the button that is ALREADY current fires onReselect (no value change) —
            // used by the WT wave button to open the table picker on a second tap. A tap on any
            // other button is a normal selection.
            b->onClick = [this, idx] { if (choice->getIndex() == idx && onReselect) onReselect (idx); else setIndex (idx); };
            b->setTooltip (choice->getName (128));      // hover -> full parameter name
            addAndMakeVisible (b);
            listenForLearnGestures (*b);
        }
        setTooltipFromParam (apvts);
        attachment = std::make_unique<juce::ParameterAttachment> (
            *choice, [this] (float) { refresh(); }, nullptr);
        attachment->sendInitialUpdate();
    }

    void paint (juce::Graphics& g) override { paintLearnDecorations (g); }

    // Fired when the currently-selected button is tapped again (no value change). Optional;
    // the WT wave button uses it to open the table picker on a second tap.
    std::function<void (int)> onReselect;

    void resized() override
    {
        auto r = getLocalBounds();
        const int n = buttons.size();
        if (n == 0) return;
        for (int i = 0; i < n; ++i)
        {
            auto cell = juce::Rectangle<int> (r.getX() + i * r.getWidth() / n, r.getY(),
                                              r.getWidth() / n, r.getHeight());
            buttons[i]->setBounds (cell.reduced (2));
        }
    }

private:
    void setIndex (int i)
    {
        choice->beginChangeGesture();
        choice->setValueNotifyingHost (choice->convertTo0to1 ((float) i));
        choice->endChangeGesture();
        refresh();
    }
    void refresh()
    {
        const int current = choice->getIndex();
        for (int i = 0; i < buttons.size(); ++i)
            buttons[i]->setToggleState (i == current, juce::dontSendNotification);
    }

    juce::AudioParameterChoice* choice = nullptr;
    juce::StringArray labels;
    juce::OwnedArray<juce::TextButton> buttons;
    std::unique_ptr<juce::ParameterAttachment> attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HSelector)
};

// ---------------------------------------------------------------------------
// LFO shape picker drawn as four stacked waveform ICONS (Triangle / Sine / Square /
// S&H) bound to a choice parameter — the R2 LFO grammar. Tap an icon to select.
// MIDI-learnable + focus-refusing.
class ShapeSelector : public LearnableComponent
{
public:
    ShapeSelector (juce::AudioProcessorValueTreeState& apvts, const juce::String& pid,
                   MidiLearnManager& learnMgr)
        : LearnableComponent (learnMgr, pid)
    {
        choice = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (pid));
        jassert (choice != nullptr);
        setTooltipFromParam (apvts);                    // hover -> full parameter name
        attachment = std::make_unique<juce::ParameterAttachment> (
            *choice, [this] (float) { repaint(); }, nullptr);
        attachment->sendInitialUpdate();
    }

    // Draw one waveform icon (0 Tri, 1 Sin, 2 Sqr, 3 S&H) filling r.
    static void drawIcon (juce::Graphics& g, juce::Rectangle<int> r, int kind, bool on)
    {
        g.setColour (on ? VASynthLookAndFeel::accent() : VASynthLookAndFeel::track());
        g.fillRoundedRectangle (r.toFloat(), 4.0f);
        auto a = r.toFloat().reduced (r.getWidth() * 0.18f, r.getHeight() * 0.28f);
        const float x0 = a.getX(), w = a.getWidth(), y0 = a.getCentreY(), h = a.getHeight() * 0.5f;
        g.setColour (on ? juce::Colour (0xff0e1319) : VASynthLookAndFeel::ink());
        juce::Path p;
        if (kind == 0)      { p.startNewSubPath (x0, y0); p.lineTo (x0+w*0.25f, y0-h); p.lineTo (x0+w*0.75f, y0+h); p.lineTo (x0+w, y0); }
        else if (kind == 1) { p.startNewSubPath (x0, y0); for (int i = 1; i <= 20; ++i) { float t = i/20.0f; p.lineTo (x0+w*t, y0 - std::sin (t*6.283f)*h); } }
        else if (kind == 2) { p.startNewSubPath (x0, y0+h); p.lineTo (x0, y0-h); p.lineTo (x0+w*0.5f, y0-h); p.lineTo (x0+w*0.5f, y0+h); p.lineTo (x0+w, y0+h); p.lineTo (x0+w, y0-h); }
        else                { const float s[5] { 0.3f,-0.6f,0.5f,-0.2f,0.7f }; float px = x0; for (int i = 0; i < 5; ++i) { float ny = y0 - s[i]*h; p.startNewSubPath (px, ny); p.lineTo (px+w/5.0f, ny); px += w/5.0f; } }
        g.strokePath (p, juce::PathStrokeType (1.6f));
    }

    void paint (juce::Graphics& g) override
    {
        const int cur = choice != nullptr ? choice->getIndex() : 0;
        const int n = 4;
        const int ih = (getHeight() - (n - 1) * gap) / n;
        for (int k = 0; k < n; ++k)
        {
            cells[(std::size_t) k] = juce::Rectangle<int> (0, k * (ih + gap), getWidth(), ih);
            drawIcon (g, cells[(std::size_t) k], k, k == cur);
        }
        paintLearnDecorations (g);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        LearnableComponent::mouseUp (e);
        if (e.mods.isPopupMenu() || e.getDistanceFromDragStart() > 8) return;
        for (int k = 0; k < 4; ++k)
            if (cells[(std::size_t) k].contains (e.getPosition()) && choice != nullptr)
            {
                choice->beginChangeGesture();
                choice->setValueNotifyingHost (choice->convertTo0to1 ((float) k));
                choice->endChangeGesture();
                repaint();
                return;
            }
    }

private:
    static constexpr int gap = 3;
    juce::AudioParameterChoice* choice = nullptr;
    std::array<juce::Rectangle<int>, 4> cells {};
    std::unique_ptr<juce::ParameterAttachment> attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ShapeSelector)
};
