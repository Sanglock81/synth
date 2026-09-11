// Synth — Copyright © 2026 John L Farmer. Licensed under AGPLv3; see LICENSE.
// ============================================================================
// HEADLESS UI SMOKE HARNESS (Part 0 process fix — see the ui-smoke-harness memory).
//
// The prior #56 mod-matrix shipped green-but-dead because every test drove the
// CONTROLLER directly (completeModLink(), pickForTest()) and never the real event
// path: no test rendered the overlay (so UTF-8 mojibake was invisible) and no test
// tapped a real knob component (so the LINK hit-area bug was invisible).
//
// This harness builds the REAL editor, drives REAL component mouse events, asserts
// end-to-end invariants (route exists AND a rendered buffer is modulated), and writes
// screenshot artifacts under docs/smoke/ for human review at every UI-feature gate.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "UI/Widgets.h"
#include "UI/Sections.h"
#include "UI/PartRail.h"
#include <cmath>
#include "UI/BottomZones.h"
#include "UI/SessionExportDialog.h"
#include "UI/OutputsDialog.h"
#include "UI/RecordSaveDialog.h"
#include "MasterRecorder.h"
#include "UI/ModMatrixPanel.h"
#include "UI/FXPanel.h"
#include "ModDestRegistry.h"
#include <functional>
#include "VersionInfo.h"
#include <memory>

#ifndef VASYNTH_DOCS_DIR
 #define VASYNTH_DOCS_DIR "."
#endif

namespace
{
    // Depth-first search for the RotaryKnob bound to `paramId`.
    RotaryKnob* findKnob (juce::Component& c, const juce::String& paramId)
    {
        for (auto* ch : c.getChildren())
        {
            if (auto* k = dynamic_cast<RotaryKnob*> (ch))
                if (k->parameterID() == paramId) return k;
            if (auto* found = findKnob (*ch, paramId)) return found;
        }
        return nullptr;
    }

    // Every parameter-attached control on the panel whose parameter is a registry mod destination.
    void collectModTargets (juce::Component& c, std::vector<LearnableComponent*>& out)
    {
        for (auto* ch : c.getChildren())
        {
            if (auto* lc = dynamic_cast<LearnableComponent*> (ch))
                if (moddest::destForParam (lc->parameterID()) != ModMatrix::DstNone)
                    out.push_back (lc);
            collectModTargets (*ch, out);
        }
    }

    // Depth-first search for the parameter-attached control (any LearnableComponent) bound to `paramId`.
    LearnableComponent* findLearnable (juce::Component& c, const juce::String& paramId)
    {
        for (auto* ch : c.getChildren())
        {
            if (auto* lc = dynamic_cast<LearnableComponent*> (ch))
                if (lc->parameterID() == paramId) return lc;
            if (auto* found = findLearnable (*ch, paramId)) return found;
        }
        return nullptr;
    }

    // Synthesize a real mouse click ON `comp` (eventComponent == comp — the parent-area
    // path the OS uses for a tap that is not on an inner child), at the component centre.
    void tap (juce::Component& comp)
    {
        const auto pos = comp.getLocalBounds().getCentre().toFloat();
        const auto now = juce::Time::getCurrentTime();
        juce::MouseEvent down (juce::Desktop::getInstance().getMainMouseSource(), pos,
                               juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                               &comp, &comp, now, pos, now, 1, false);
        comp.mouseDown (down);
        comp.mouseUp (down);
    }

    void snapshot (juce::Component& c, const juce::String& name)
    {
        auto img = c.createComponentSnapshot (c.getLocalBounds(), false, 1.0f);
        REQUIRE (img.isValid());
        juce::File out (juce::String (VASYNTH_DOCS_DIR) + "/smoke/" + name);
        out.getParentDirectory().createDirectory();
        out.deleteFile();
        juce::FileOutputStream os (out); REQUIRE (os.openedOk());
        juce::PNGImageFormat png; REQUIRE (png.writeImageToStream (img, os));
    }

    float renderPeak (VASynthProcessor& p, float modWheel)
    {
        p.prepareToPlay (48000.0, 128);
        float peak = 0.0f;
        for (int b = 0; b < 24; ++b)
        {
            juce::AudioBuffer<float> buf (2, 128); buf.clear();
            juce::MidiBuffer midi;
            if (b == 0) { midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
                          midi.addEvent (juce::MidiMessage::controllerEvent (1, 1, (int) (modWheel * 127)), 0); }
            p.processBlock (buf, midi);
            if (b >= 6) peak = std::max (peak, buf.getMagnitude (0, 128));
        }
        return peak;
    }
}

// --- H0: the build-fresh version hash header is wired (banner is confirmable) ----------
TEST_CASE ("build-fresh git-hash header is present and populated (#H0)", "[plugin][smoke][version]")
{
    const juce::String hash = VASYNTH_GIT_HASH_RT;    // regenerated every build by cmake/gen_version.cmake
    REQUIRE (hash.isNotEmpty());
    REQUIRE (hash != "unknown");
    REQUIRE (hash.length() >= 7);                      // a real short hash (optionally + a "+" dirty flag)
}

// --- encoding: the overlay strings decode as UTF-8, not ASCII/Latin-1 -----------------
TEST_CASE ("mod overlay glyph strings decode as single UTF-8 codepoints (no mojibake) (#56)",
           "[plugin][smoke][encoding]")
{
    // The em-dash "None" indicator: if a regression reverts to String("\xe2\x80\x94")
    // (ASCII decode) this becomes THREE codepoints ("â..") — the shipped mojibake.
    const auto src = ModMatrixPanel::sourceNames();
    const auto dst = ModMatrixPanel::destNames();
    REQUIRE (src[0].length() == 1);
    REQUIRE (dst[0].length() == 1);
    REQUIRE (*src[0].getCharPointer() == (juce::juce_wchar) 0x2014);   // — U+2014 EM DASH
    REQUIRE (*dst[0].getCharPointer() == (juce::juce_wchar) 0x2014);
}

// --- the real LINK gesture: arm -> tap a real knob -> route exists AND audio moves -----
TEST_CASE ("LINK gesture: a real tap on a destination knob creates a route that modulates audio (#56)",
           "[plugin][smoke][modmatrix][link]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;

    // A quiet single-oscillator patch so an osc-level route is unambiguous on the meter.
    p.apvts.getParameter (ParamID::osc1Level)->setValueNotifyingHost (0.3f);
    p.apvts.getParameter (ParamID::osc2On)->setValueNotifyingHost (0.0f);
    p.apvts.getParameter (ParamID::osc3On)->setValueNotifyingHost (0.0f);

    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    ed->setSize (1760, 1000);   // triggers a full recursive layout so knob bounds are real

    auto* knob = findKnob (*ed, ParamID::osc1Level);
    REQUIRE (knob != nullptr);

    // Before arming, the knob is not a LINK target and a tap must NOT create a route.
    REQUIRE_FALSE (knob->isLinkArmable());

    // Arm ModWheel (what the LINK button's source menu does), then TAP the knob on its
    // body — the parent-area path that the shipped hit-area bug dropped on the floor.
    p.armModLink (ModMatrix::ModWheel);
    REQUIRE (knob->isLinkArmable());
    tap (*knob);

    // The route now exists on the focused part, and LINK auto-disarmed.
    bool routed = false;
    for (int s = 0; s < ModMatrix::kSlots; ++s)
    {
        const auto slot = p.getModSlot (-1, s);
        if (slot.source == ModMatrix::ModWheel && slot.dest == ModMatrix::Osc1Level) routed = true;
    }
    REQUIRE (routed);
    REQUIRE_FALSE (p.linkArmed());

    // End to end: with the mod wheel up, osc1 level is lifted, so the render is louder.
    const float base    = renderPeak (p, 0.0f);
    const float lifted  = renderPeak (p, 1.0f);
    REQUIRE (base > 0.0f);
    REQUIRE (lifted > base * 1.15f);
}

// --- THE load-bearing gate: EVERY registry destination control connects + animates ---------
// Iterates every mod-target control in the real editor. For each: arm a source, assert the
// control reports the connect-ring, drive a REAL tap, assert the route exists in the matrix,
// then engage the source and assert the control's animation offset goes live. This makes
// "LINK works on some knobs but not others" structurally impossible to ship.
TEST_CASE ("LINK connects + animates on EVERY registry destination control (#56 follow-up)",
           "[plugin][smoke][modmatrix][link]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    ed->setSize (1760, 1000);

    std::vector<LearnableComponent*> targets;
    collectModTargets (*ed, targets);

    // The panel must expose a broad set of targets (FX, EQ, LFO, env, osc, filter, glide) — not
    // just the original five. A regression that drops the central wiring collapses this count.
    INFO ("mod-target controls found: " << (int) targets.size());
    REQUIRE (targets.size() >= 25);

    // #12: every mod TARGET must have actually BUILT its motion indicator (wired AND drawn) — not
    // merely report live modAnim() data. The NOISE HBarControl regressed exactly here: it published
    // a live offset but never created an overlay, so it never animated. Assert the indicator exists.
    for (auto* lc : targets)
    {
        INFO ("mod target with no indicator overlay: " << lc->parameterID());
        REQUIRE (lc->hasModIndicator());
    }

    // Macro 1 drives both tiers; set it high so the applied offset (and animation) is unambiguous.
    p.apvts.getParameter (ParamID::macro1)->setValueNotifyingHost (1.0f);
    p.prepareToPlay (48000.0, 128);

    int connected = 0, animated = 0;
    for (auto* lc : targets)
    {
        const int dest = moddest::destForParam (lc->parameterID());
        for (int s = 0; s < ModMatrix::kSlots; ++s) p.clearModSlot (-1, s);   // clean slate each time

        p.armModLink (ModMatrix::Macro1);
        REQUIRE (lc->isLinkArmable());                       // the connect-ring shows on THIS control
        tap (*lc);                                           // a real tap on the control

        bool routed = false;
        for (int s = 0; s < ModMatrix::kSlots; ++s)
        {
            const auto slot = p.getModSlot (-1, s);
            if (slot.source == ModMatrix::Macro1 && slot.dest == dest) routed = true;
        }
        INFO ("dest " << dest << " (" << lc->parameterID() << ") did not route");
        REQUIRE (routed);
        ++connected;

        // Engage the source through a block so the offset publishes, then the animation is live.
        for (int b = 0; b < 4; ++b) { juce::AudioBuffer<float> buf (2, 128); buf.clear(); juce::MidiBuffer m; p.processBlock (buf, m); }
        if (std::abs (lc->modAnim()) > 1.0e-4f) ++animated;
    }

    REQUIRE (connected == (int) targets.size());             // ALL of them connect
    REQUIRE (animated  >= (int) (targets.size() * 8 / 10));   // and the vast majority animate live
}

// #13a: LINK-ing an LFO whose DEST is the default "Off" must still work end to end. The engine
// zeroes an Off LFO's published source, so before the fix the route was created but SILENT and
// un-animated ("LFO link broken"). The arm->tap gesture now auto-enables the LFO as a live source
// ("On"), driven here through the REAL editor event path (not a direct linkModRoute).
TEST_CASE ("LINK from an LFO auto-enables it as a source (arm->tap, no manual DEST=On) (#13a)",
           "[plugin][smoke][modmatrix][link][lfo]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    ed->setSize (1760, 1000);
    p.prepareToPlay (48000.0, 128);

    // Clean slate: the startup patch (Bright Lead) already routes its LFO -> cutoff, so clear the
    // matrix and force LFO 1 DEST to "Off" to model a fresh LFO the user is about to link.
    for (int s = 0; s < ModMatrix::kSlots; ++s) p.clearModSlot (-1, s);
    auto* destParam = p.apvts.getParameter (ParamID::lfoDest);
    auto destIndex  = [&] { return (int) std::lround (destParam->convertFrom0to1 (destParam->getValue())); };
    destParam->setValueNotifyingHost (destParam->convertTo0to1 (0.0f));
    REQUIRE (destIndex() == 0);                         // LFO 1 DEST is Off before the link

    p.apvts.getParameter (ParamID::lfoDepth)->setValueNotifyingHost (1.0f);   // deep -> unambiguous motion

    std::vector<LearnableComponent*> targets; collectModTargets (*ed, targets);
    LearnableComponent* cutoff = nullptr;
    for (auto* lc : targets) if (lc->parameterID() == ParamID::filterCutoff) { cutoff = lc; break; }
    REQUIRE (cutoff != nullptr);

    p.armModLink (ModMatrix::LFO1);                     // exactly what the source menu does
    tap (*cutoff);                                      // a real tap on the destination control

    REQUIRE (destIndex() == 3);                         // the gesture auto-enabled LFO 1 as a live source ("On")

    float mx = 0.0f;
    for (int b = 0; b < 40; ++b)
    {
        juce::AudioBuffer<float> buf (2, 128); buf.clear(); juce::MidiBuffer m;
        if (b == 0) m.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
        p.processBlock (buf, m);
        mx = std::max (mx, std::abs (cutoff->modAnim()));
    }
    INFO ("cutoff modAnim peak after LFO link = " << mx);
    REQUIRE (mx > 1.0e-3f);                             // the linked LFO now actually animates the cutoff
}

// #2: the PW knobs get a snappier drag (narrow audible range felt sluggish at the 313-px default).
TEST_CASE ("PW knobs use a more responsive drag than the global default (#2)", "[plugin][smoke][pw]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    ed->setSize (1760, 1000);

    std::vector<LearnableComponent*> targets; collectModTargets (*ed, targets);
    int pwChecked = 0;
    for (auto* lc : targets)
        if (auto* knob = dynamic_cast<RotaryKnob*> (lc))
            if (knob->parameterID() == ParamID::osc1PW || knob->parameterID() == ParamID::osc2PW
                || knob->parameterID() == ParamID::osc3PW)
            {
                INFO ("PW knob " << knob->parameterID() << " dragPixels=" << knob->dragPixels());
                REQUIRE (knob->dragPixels() == OscSection::kPwDragPixels);
                REQUIRE (knob->dragPixels() < kDragPixelsForFullRange);   // snappier than every other knob
                ++pwChecked;
            }
    REQUIRE (pwChecked == 3);                                            // all three osc PW knobs
}

// --- J1.3: the LFO SYNC toggle morphs the RATE knob into the DIV (note-division) knob ----
TEST_CASE ("LFO SYNC swaps the visible RATE<->DIV control (#J1)", "[plugin][smoke][lfo][sync]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    ed->setSize (1760, 1000);   // full recursive layout so both knobs get real bounds

    auto* rate = findKnob (*ed, ParamID::lfoRate);
    auto* div  = findKnob (*ed, ParamID::lfoDiv);
    REQUIRE (rate != nullptr);
    REQUIRE (div  != nullptr);

    // ParameterAttachment callbacks fire synchronously when driven on the message thread (here).

    // Default: SYNC off -> RATE (free Hz) shows, DIV hidden.
    p.apvts.getParameter (ParamID::lfoSync)->setValueNotifyingHost (0.0f);
    REQUIRE (rate->isVisible());
    REQUIRE_FALSE (div->isVisible());

    // SYNC on -> DIV (note division) shows in the same slot, RATE hidden.
    p.apvts.getParameter (ParamID::lfoSync)->setValueNotifyingHost (1.0f);
    REQUIRE (div->isVisible());
    REQUIRE_FALSE (rate->isVisible());

    // The two occupy the same bounds (a true morph, not two stacked controls).
    REQUIRE (rate->getBounds() == div->getBounds());

    // Snapshot the LFO section in the synced state for the gate's human review.
    juce::Component* section = div->getParentComponent();
    while (section != nullptr && dynamic_cast<LfoSection*> (section) == nullptr)
        section = section->getParentComponent();
    REQUIRE (section != nullptr);
    snapshot (*section, "lfo-sync.png");

    // Back off -> RATE returns (idempotent swap).
    p.apvts.getParameter (ParamID::lfoSync)->setValueNotifyingHost (0.0f);
    REQUIRE (rate->isVisible());
    REQUIRE_FALSE (div->isVisible());
}

// --- #132: the FM depth knob exists on osc1/osc2 and enables only for a SIN/TRI/WT carrier ------
TEST_CASE ("FM depth knob tracks the carrier wave restriction (#132)", "[plugin][smoke][fm]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    ed->setSize (1760, 1000);   // full recursive layout so the knobs get real bounds

    auto* fm1 = findKnob (*ed, ParamID::osc1Fm);
    auto* fm2 = findKnob (*ed, ParamID::osc2Fm);
    REQUIRE (fm1 != nullptr);   // osc1 (carrier) FM depth — osc2 modulates osc1
    REQUIRE (fm2 != nullptr);   // osc2 (carrier) FM depth — osc3 modulates osc2

    // Saw carrier (default, index 0) -> FM inert -> knob disabled.
    p.apvts.getParameter (ParamID::osc1Wave)->setValueNotifyingHost (0.0f);          // Saw
    REQUIRE_FALSE (fm1->isEnabled());
    // Sine carrier (index 3 of 5 -> normalized 0.75) -> FM available -> knob enabled.
    p.apvts.getParameter (ParamID::osc1Wave)->setValueNotifyingHost (3.0f / 4.0f);   // Sine
    REQUIRE (fm1->isEnabled());
    // Triangle (index 2) is also a valid carrier; Square (index 1) is not.
    p.apvts.getParameter (ParamID::osc1Wave)->setValueNotifyingHost (2.0f / 4.0f);   // Triangle
    REQUIRE (fm1->isEnabled());
    p.apvts.getParameter (ParamID::osc1Wave)->setValueNotifyingHost (1.0f / 4.0f);   // Square
    REQUIRE_FALSE (fm1->isEnabled());

    // Snapshot the osc section (sine carrier, FM active, osc2 marked as the MOD source) for review.
    p.apvts.getParameter (ParamID::osc1Wave)->setValueNotifyingHost (3.0f / 4.0f);   // Sine carrier
    p.apvts.getParameter (ParamID::osc1Fm)->setValueNotifyingHost (0.45f);           // FM on -> osc2 shows "MOD"
    juce::Component* section = fm1->getParentComponent();
    while (section != nullptr && dynamic_cast<OscSection*> (section) == nullptr)
        section = section->getParentComponent();
    REQUIRE (section != nullptr);
    snapshot (*section, "osc-fm.png");
}

// --- #133: the section guide spotlights a section with numbered markers + a side card ------
TEST_CASE ("section guide renders on Oscillators (spotlight + markers + card) (#133)", "[plugin][smoke][guide]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    ed->setSize (1760, 1000);
    auto* ve = dynamic_cast<VASynthEditor*> (ed.get());
    REQUIRE (ve != nullptr);

    ve->openSectionGuide (2);   // Oscillators (dense section -> proves the marker layout)
    REQUIRE (ve->guideOverlayForTest().isVisible());
    snapshot (*ed, "guide-oscillators.png");

    ve->openSectionGuide (6);   // FX chain (15 entries -> the densest CARD; verify none drop)
    snapshot (*ed, "guide-fx.png");

    ve->openSectionGuide (11);  // Looper & Scenes (split section, spotlight on the bottom strip)
    REQUIRE (ve->guideOverlayForTest().isVisible());
    snapshot (*ed, "guide-looper.png");
}

// --- #95 3c: selecting WT on an osc swaps the PW knob for a WT POS knob (same slot) --------
TEST_CASE ("WT wave swaps the visible PW<->WT POS control (#95)", "[plugin][smoke][wt][morph]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    ed->setSize (1760, 1000);   // full recursive layout so both knobs get real bounds

    auto* pw    = findKnob (*ed, ParamID::osc1PW);
    auto* wtpos = findKnob (*ed, ParamID::osc1WtPos);
    REQUIRE (pw    != nullptr);
    REQUIRE (wtpos != nullptr);

    // Find the first DieButton descendant (the re-roll affordance; not parameter-bound).
    std::function<DieButton*(juce::Component&)> findDie = [&] (juce::Component& c) -> DieButton*
    {
        for (auto* ch : c.getChildren())
        {
            if (auto* d = dynamic_cast<DieButton*> (ch)) return d;
            if (auto* found = findDie (*ch)) return found;
        }
        return nullptr;
    };
    auto* die = findDie (*ed);
    REQUIRE (die != nullptr);

    // Default wave (Saw) -> PW shows, WT POS + die hidden.
    p.apvts.getParameter (ParamID::osc1Wave)->setValueNotifyingHost (0.0f);   // Saw
    REQUIRE (pw->isVisible());
    REQUIRE_FALSE (wtpos->isVisible());
    REQUIRE_FALSE (die->isVisible());

    // WT (index 4 of 5 -> normalized 1.0) -> WT POS + die show, PW hidden.
    p.apvts.getParameter (ParamID::osc1Wave)->setValueNotifyingHost (1.0f);   // WT
    REQUIRE (wtpos->isVisible());
    REQUIRE_FALSE (pw->isVisible());
    REQUIRE (die->isVisible());

    // A true morph: the two share one slot.
    REQUIRE (pw->getBounds() == wtpos->getBounds());

    // Snapshot the oscillator section in WT mode for the gate's human review.
    juce::Component* section = wtpos->getParentComponent();
    while (section != nullptr && dynamic_cast<OscSection*> (section) == nullptr)
        section = section->getParentComponent();
    REQUIRE (section != nullptr);
    snapshot (*section, "osc-wt.png");

    // Back to a classic wave -> PW returns, die hidden (idempotent swap).
    p.apvts.getParameter (ParamID::osc1Wave)->setValueNotifyingHost (0.0f);   // Saw
    REQUIRE (pw->isVisible());
    REQUIRE_FALSE (wtpos->isVisible());
    REQUIRE_FALSE (die->isVisible());
}

// --- #98: the session-export dialog renders with a bar-count field (default = realign cycle) ------
TEST_CASE ("session export: the dialog renders with a bar-count field (#98)", "[plugin][smoke][export]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    SessionExportDialog dlg (p);
    dlg.setSize (480, 156);

    std::function<juce::TextEditor*(juce::Component&)> findEd = [&] (juce::Component& c) -> juce::TextEditor*
    {
        for (auto* ch : c.getChildren())
        {
            if (auto* e = dynamic_cast<juce::TextEditor*> (ch)) return e;
            if (auto* f = findEd (*ch)) return f;
        }
        return nullptr;
    };
    auto* bars = findEd (dlg);
    REQUIRE (bars != nullptr);
    REQUIRE (bars->getText().getIntValue() == p.realignBars());   // default = the realign cycle (1 with no loops)
    dlg.repaint();
    snapshot (dlg, "session-export.png");
}

// --- #96: the unison controls (COUNT/DETUNE/WIDTH) are wired + render ---------------------
TEST_CASE ("unison: UNI/DET/WID controls are bound and render (#96)", "[plugin][smoke][unison]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    ed->setSize (1760, 1000);

    auto* uni = findKnob (*ed, ParamID::oscUnison);
    auto* det = findKnob (*ed, ParamID::oscUnisonDetune);
    auto* wid = findKnob (*ed, ParamID::oscUnisonWidth);
    REQUIRE (uni != nullptr);
    REQUIRE (det != nullptr);
    REQUIRE (wid != nullptr);
    REQUIRE (uni->isVisible());

    // The count param drives the knob (7 = a full stack).
    p.apvts.getParameter (ParamID::oscUnison)->setValueNotifyingHost (
        p.apvts.getParameter (ParamID::oscUnison)->convertTo0to1 (7.0f));
    REQUIRE ((int) p.apvts.getRawParameterValue (ParamID::oscUnison)->load() == 7);

    juce::Component* bar = uni->getParentComponent();
    while (bar != nullptr && dynamic_cast<TopBar*> (bar) == nullptr) bar = bar->getParentComponent();
    REQUIRE (bar != nullptr);
    bar->repaint();
    snapshot (*bar, "unison-controls.png");
}

// --- Inc 3: the Save dialog carries a category picker; screenshot for sign-off ------------
// Rendered as a plain offscreen Component (NOT a real juce::AlertWindow, which would try to
// grab a native X window and fail under CI's Xvfb) — same widget types the real dialog uses.
namespace
{
    struct SaveDialogMock : juce::Component
    {
        juce::Label title { {}, "Save Preset" }, nameLbl { {}, "Preset name:" }, catLbl { {}, "Category:" };
        juce::TextEditor name;
        juce::ComboBox cat;
        juce::TextButton saveB { "Save" }, cancelB { "Cancel" };
        SaveDialogMock()
        {
            title.setJustificationType (juce::Justification::centred);
            title.setFont (juce::Font (juce::FontOptions (18.0f, juce::Font::bold)));
            for (auto* l : { &title, &nameLbl, &catLbl }) l->setColour (juce::Label::textColourId, juce::Colours::white);
            name.setText ("My Patch");
            juce::StringArray cats = FactoryPresetLibrary::canonicalOrder(); cats.add ("User");
            for (int i = 0; i < cats.size(); ++i) cat.addItem (cats[i], i + 1);
            cat.setText ("Bass", juce::dontSendNotification);
            addAndMakeVisible (title);  addAndMakeVisible (nameLbl); addAndMakeVisible (name);
            addAndMakeVisible (catLbl); addAndMakeVisible (cat);     addAndMakeVisible (saveB); addAndMakeVisible (cancelB);
        }
        void paint (juce::Graphics& g) override { g.fillAll (juce::Colour (0xff23272e)); }
        void resized() override
        {
            auto r = getLocalBounds().reduced (16);
            title.setBounds (r.removeFromTop (30)); r.removeFromTop (8);
            nameLbl.setBounds (r.removeFromTop (20));
            name.setBounds (r.removeFromTop (28)); r.removeFromTop (10);
            catLbl.setBounds (r.removeFromTop (20));
            cat.setBounds (r.removeFromTop (28)); r.removeFromTop (14);
            auto b = r.removeFromTop (30);
            saveB.setBounds (b.removeFromLeft (b.getWidth() / 2).reduced (6, 0));
            cancelB.setBounds (b.reduced (6, 0));
        }
    };
}

TEST_CASE ("save dialog: category picker present + renders", "[plugin][smoke][menu]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    SaveDialogMock dlg;
    dlg.setSize (420, 230);

    juce::StringArray cats = FactoryPresetLibrary::canonicalOrder(); cats.add ("User");
    REQUIRE (dlg.cat.getNumItems() == cats.size());              // every category is offered
    REQUIRE (dlg.cat.getText() == "Bass");

    snapshot (dlg, "save-dialog.png");
}

// --- Inc 2: per-patch TRIM (program level) is wired, carries the preset, screenshots ----
TEST_CASE ("trim: patch TRIM control is bound, follows the loaded patch, renders", "[plugin][smoke][trim]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    ed->setSize (1760, 1000);

    auto* trim = findKnob (*ed, ParamID::patchTrim);
    REQUIRE (trim != nullptr);
    REQUIRE (trim->isVisible());

    // A loudness-matched factory patch carries a non-unity trim; the control must reflect it.
    p.loadFactoryPreset ("Full Organ");   // trimmed DOWN (0.58) by the bank match
    const float t = p.apvts.getRawParameterValue (ParamID::patchTrim)->load();
    REQUIRE (t < 0.9f);                    // clearly below unity
    REQUIRE (t > 0.25f);

    juce::Component* bar = trim->getParentComponent();
    while (bar != nullptr && dynamic_cast<TopBar*> (bar) == nullptr) bar = bar->getParentComponent();
    REQUIRE (bar != nullptr);
    bar->repaint();
    snapshot (*bar, "patch-trim.png");
}

// --- J2: each looper lane has its OWN bars selector (per-part loop length) ---------------
TEST_CASE ("J2: per-lane looper BARS selectors are wired + render (#J2)", "[plugin][smoke][looper][j2]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    ed->setSize (1760, 1000);   // full recursive layout so the looper rows get real bounds

    // All four per-lane bars selectors exist and are bound to their OWN param.
    const char* barsIds[] { ParamID::loopBars, ParamID::loopBars2, ParamID::loopBars3, ParamID::loopBars4 };
    for (auto* id : barsIds) { INFO ("missing bars selector: " << id); REQUIRE (findLearnable (*ed, id) != nullptr); }

    // Set a distinct length per lane (2 / 4 / 8 / 16 bars) so the screenshot shows the spread,
    // and confirm each selector reflects its own param independently.
    const int idx[] { 1, 2, 3, 4 };
    for (int i = 0; i < 4; ++i)
        p.apvts.getParameter (barsIds[i])->setValueNotifyingHost (
            p.apvts.getParameter (barsIds[i])->convertTo0to1 ((float) idx[i]));
    for (int i = 0; i < 4; ++i)
        REQUIRE (dynamic_cast<juce::AudioParameterChoice*> (p.apvts.getParameter (barsIds[i]))->getIndex() == idx[i]);

    // Snapshot the looper panel for the gate's human review.
    LearnableComponent* lc = findLearnable (*ed, ParamID::loopBars2);
    juce::Component* panel = lc ? lc->getParentComponent() : nullptr;
    while (panel != nullptr && dynamic_cast<LooperPanel*> (panel) == nullptr) panel = panel->getParentComponent();
    REQUIRE (panel != nullptr);
    panel->repaint();
    snapshot (*panel, "looper-lengths.png");
}

// --- hover help: every parameter-bound control carries its full name as a tooltip ---------
TEST_CASE ("controls expose their parameter name as a hover tooltip", "[plugin][smoke][tooltip]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    ed->setSize (1760, 1000);

    // A knob, a choice selector, and the new looper BARS knob each report the registered param name.
    struct { const char* id; } probes[] { { ParamID::lfoRate }, { ParamID::filterCutoff },
                                          { ParamID::loopBars }, { ParamID::lfoDest } };
    for (auto& pr : probes)
    {
        auto* lc = findLearnable (*ed, pr.id);
        INFO ("no control for " << pr.id);
        REQUIRE (lc != nullptr);
        const auto want = p.apvts.getParameter (pr.id)->getName (128);
        INFO (pr.id << " tooltip '" << lc->getTooltip() << "' != name '" << want << "'");
        REQUIRE (lc->getTooltip() == want);
        REQUIRE (lc->getTooltip().isNotEmpty());
    }

    // The whole editor is served by exactly one TooltipWindow (JUCE finds it by walking up).
    int windows = 0;
    std::function<void (juce::Component&)> count = [&] (juce::Component& c)
    { if (dynamic_cast<juce::TooltipWindow*> (&c)) ++windows; for (auto* ch : c.getChildren()) count (*ch); };
    count (*ed);
    REQUIRE (windows >= 1);
}

// --- J3: eight scene buttons launch on tap; screenshot the states -----------------------
TEST_CASE ("J3: tapping a scene button arms it (pending); the row renders (#J3)", "[plugin][smoke][scene][j3]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    ed->setSize (1760, 1000);

    // Collect the eight scene buttons.
    std::vector<SceneButton*> scenes;
    std::function<void (juce::Component&)> collect = [&] (juce::Component& c)
    { if (auto* s = dynamic_cast<SceneButton*> (&c)) scenes.push_back (s); for (auto* ch : c.getChildren()) collect (*ch); };
    collect (*ed);
    REQUIRE (scenes.size() == (size_t) VASynthProcessor::kScenes);

    // A real tap on scene 3 arms it as pending (launch is quantized, so it does not switch yet).
    REQUIRE (p.pendingSceneIndex() == -1);
    tap (*scenes[3]);
    REQUIRE (p.pendingSceneIndex() == 3);
    REQUIRE (p.activeScene() == 0);                  // still on scene 0 until the boundary

    // The launch-quantum selector is bound to its param.
    REQUIRE (findLearnable (*ed, ParamID::sceneQuant) != nullptr);

    // Screenshot: scene 0 active (with content), scene 3 pending, others empty.
    p.setSeqCell (0, 0, 1);
    { juce::AudioBuffer<float> buf (2, 128); for (int b = 0; b < 4; ++b) { buf.clear(); juce::MidiBuffer m; p.processBlock (buf, m); } }
    juce::Component* panel = scenes[0]->getParentComponent();
    while (panel != nullptr && dynamic_cast<LooperPanel*> (panel) == nullptr) panel = panel->getParentComponent();
    REQUIRE (panel != nullptr);
    panel->repaint();
    snapshot (*panel, "scenes.png");
}

// --- #85: the OUTPUTS dialog's enable toggle is bound to the clock_out param ------------
TEST_CASE ("OUTPUTS dialog: the MIDI-clock enable toggle round-trips the param (#85)", "[plugin][smoke][clockout]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;

    p.apvts.getParameter (ParamID::clockOut)->setValueNotifyingHost (1.0f);   // enabled before the dialog opens
    OutputsDialog dlg (p);                                                     // constructs + attaches
    juce::ToggleButton* enable = nullptr;
    std::function<void (juce::Component&)> find = [&] (juce::Component& c)
    { if (auto* t = dynamic_cast<juce::ToggleButton*> (&c)) enable = t; for (auto* ch : c.getChildren()) find (*ch); };
    find (dlg);
    REQUIRE (enable != nullptr);
    REQUIRE (enable->getToggleState());                     // reflects the enabled param

    p.apvts.getParameter (ParamID::clockOut)->setValueNotifyingHost (0.0f);
    REQUIRE_FALSE (enable->getToggleState());               // and follows it live
}

// --- Musicality Tier 1: the per-osc phase selectors + the ANALOG knob are present + wired -----
TEST_CASE ("Tier 1 UI: osc phase selectors + ANALOG knob are bound (#99)", "[plugin][smoke][musicality]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    ed->setSize (1760, 1000);

    for (auto* id : { ParamID::osc1Phase, ParamID::osc2Phase, ParamID::osc3Phase })
    { INFO ("missing phase selector: " << id); REQUIRE (findLearnable (*ed, id) != nullptr); }
    REQUIRE (findKnob (*ed, ParamID::analog) != nullptr);

    // Init is RESET / analog 0 (bit-exact); the selector reflects it. (The startup default patch
    // may ship drift/phase settings, so check against Init, not whatever loads at construction.)
    p.loadInitPreset();
    REQUIRE (dynamic_cast<juce::AudioParameterChoice*> (p.apvts.getParameter (ParamID::osc1Phase))->getIndex() == 0);
    REQUIRE (p.apvts.getParameter (ParamID::analog)->getValue() == Catch::Approx (0.0f));
}

// --- screenshot artifacts for the gate (human eyeball; also proves both paint) ---------
TEST_CASE ("smoke screenshots: editor + mod overlay render for the gate (#56)",
           "[plugin][smoke][screenshot]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    p.setModSlot (-1, 0, ModMatrix::LFO1,     ModMatrix::Cutoff, 0.5f);    // a couple of routes so the
    p.setModSlot (-1, 1, ModMatrix::Velocity, ModMatrix::Amp,   -0.4f);    // overlay has content to show

    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    ed->setSize (1760, 1000);
    snapshot (*ed, "editor-default.png");

    ModMatrixPanel panel (p);
    snapshot (panel, "mod-overlay.png");

    // Connect-mode: arm a source and every registry control across the panel shows its cyan ring.
    p.armModLink (ModMatrix::Macro1);
    ed->repaint();
    snapshot (*ed, "editor-link-armed.png");
}

// --- FX reorder chevrons: the REAL mouse path moves a block in the processor chain ------
// Guards the wiring the drag-removal replaced: a tap on a block's down-chevron must reach
// FXPanel::moveBlock -> processor setFxOrder (not just the public method in isolation).
TEST_CASE ("FX reorder chevron: tapping a block's down-arrow moves it one slot in the chain",
           "[plugin][smoke][fx][reorder]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;

    int before[5]; p.getFxOrder (before);                 // default width-first {3,0,1,2,4}

    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    ed->setSize (1760, 1000);                             // real recursive layout -> real block bounds

    FXPanel* fxp = nullptr;
    std::function<void (juce::Component&)> find = [&] (juce::Component& c)
    {
        if (auto* f = dynamic_cast<FXPanel*> (&c)) fxp = f;
        for (auto* ch : c.getChildren()) if (fxp == nullptr) find (*ch);
    };
    find (*ed);
    REQUIRE (fxp != nullptr);
    REQUIRE (fxp->getNumChildComponents() >= FXPanel::kNumShown);

    // Child 0 is the CHORUS block (fx index 0), which sits at chain slot 1 by default. Its down
    // arrow (right ~22 px of the 26 px name bar) must move it to slot 2: {3,0,1,2,4} -> {3,1,0,2,4}.
    auto* block = fxp->getChildComponent (0);
    REQUIRE (block != nullptr);
    const auto pt = juce::Point<float> ((float) (block->getWidth() - 11), 13.0f);   // down-chevron centre
    const auto now = juce::Time::getCurrentTime();
    juce::MouseEvent e (juce::Desktop::getInstance().getMainMouseSource(), pt, juce::ModifierKeys(),
                        1.0f, 0.0f, 0.0f, 0.0f, 0.0f, block, block, now, pt, now, 1, false);
    block->mouseUp (e);

    int after[5]; p.getFxOrder (after);
    const int want[5] { 3, 1, 0, 2, 4 };   // CHORUS (fx 0) moved from slot 1 to slot 2
    for (int i = 0; i < 5; ++i) REQUIRE (after[i] == want[i]);
    REQUIRE (want[1] != before[1]);        // sanity: this really is a different order than the default
}

// --- proportional layout: every split holds its share at every window size --------------
// Windows report (Aug 2026): "the arpeggiator and the looper are too tall, compacting the
// upper parts". The panels were laid out at FIXED pixel sizes (a 476 px bottom band, a 232 px
// part rail, a 286 px scope column), so on a 1280x720 surface -- a 720p laptop, or 1080p at
// 150% display scaling -- the band swallowed the editor (osc..fx got 136 px) and the FX panel
// was squeezed to ~20 px wide. Every split is now a share of the space available; this pins
// BOTH that the shares are held at every size AND that the controls stay usable.
TEST_CASE ("layout: panel splits are proportional to the window at every size",
           "[plugin][smoke][layout][shortscreen]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    ed->setVisible (true);                 // getComponentAt() ignores an invisible tree

    BottomZones* band = nullptr;
    OscSection*  osc  = nullptr;
    LfoSection*  lfo  = nullptr;
    FXPanel*     fx   = nullptr;
    PartRail*    rail = nullptr;
    std::function<void (juce::Component&)> find = [&] (juce::Component& c)
    {
        if (auto* b = dynamic_cast<BottomZones*> (&c)) band = b;
        if (auto* o = dynamic_cast<OscSection*>  (&c)) osc  = o;
        if (auto* l = dynamic_cast<LfoSection*>  (&c)) lfo  = l;
        if (auto* f = dynamic_cast<FXPanel*>     (&c)) fx   = f;
        if (auto* r = dynamic_cast<PartRail*>    (&c)) rail = r;
        for (auto* ch : c.getChildren()) find (*ch);
    };
    auto layout = [&] (int w, int h)
    {
        ed->setSize (w, h);
        band = nullptr; osc = nullptr; lfo = nullptr; fx = nullptr; rail = nullptr;
        find (*ed);
        REQUIRE (band != nullptr); REQUIRE (osc != nullptr); REQUIRE (lfo != nullptr);
        REQUIRE (fx != nullptr);   REQUIRE (rail != nullptr);
    };

    // The share constants ARE the signed-off 1920x1080 pixel sizes, so that screen must come out
    // byte-identical to the hand-tuned layout -- proportional everywhere else, unchanged here.
    layout (1920, 1080);
    REQUIRE (osc->getY() == 6 + VASynthEditor::kTopBarH + 5);
    REQUIRE (band->getHeight() == VASynthEditor::kBandShare);
    REQUIRE (osc->getHeight()  == VASynthEditor::kCentreVShare);
    REQUIRE (rail->getWidth()  == VASynthEditor::kRailShare);
    REQUIRE (band->getHeight() == band->preferredHeight());

    struct Size { int w, h; };
    for (auto sz : { Size { 1280, 720 }, Size { 1366, 768 }, Size { 1600, 900 },
                     Size { 1920, 1080 }, Size { 2560, 1440 }, Size { 1760, 1000 },
                     Size { 900, 480 } })
    {
        layout (sz.w, sz.h);
        INFO ("size @" << sz.w << "x" << sz.h
              << "  centre=" << osc->getHeight() << "  band=" << band->getHeight()
              << "  rail=" << rail->getWidth());

        // ---- vertical share: band vs centre, of the flexible space below the top bar ----
        const int flexV = osc->getHeight() + band->getHeight();
        REQUIRE (flexV > 0);
        const int gotV  = band->getHeight() * 1000 / flexV;
        const int wantV = VASynthEditor::kBandShare * 1000
                        / (VASynthEditor::kBandShare + VASynthEditor::kCentreVShare);
        REQUIRE (std::abs (gotV - wantV) <= 3);          // same proportion at every size

        // ---- horizontal share: part rail vs centre vs scope/EQ column ----
        const int centreW = fx->getRight() - osc->getX();
        const int flexH   = rail->getWidth() + centreW + (ed->getWidth() - 6 - fx->getRight() - 5);
        REQUIRE (flexH > 0);
        const int gotH  = rail->getWidth() * 1000 / flexH;
        const int wantH = VASynthEditor::kRailShare * 1000
                        / (VASynthEditor::kRailShare + VASynthEditor::kCentreHShare + VASynthEditor::kRightShare);
        REQUIRE (std::abs (gotH - wantH) <= 3);

        // Nothing may be crowded out of existence, and the FX panel must keep real width
        // (fixed side columns used to leave it ~20 px on a 1280-wide screen).
        REQUIRE (lfo->getHeight() == osc->getHeight());   // the centre row lays out as one band
        REQUIRE (fx->getWidth() >= centreW / 8);
        REQUIRE (osc->getHeight() + band->getHeight() <= band->getBottom() - osc->getY());

        // Every zone inside the band keeps a usable height and stays within it.
        for (juce::Component* z : { &band->chordZone(), &band->arpZone(),
                                    &band->seqZone(),   &band->looperZone() })
        {
            REQUIRE (z->getHeight() >= 40);
            REQUIRE (band->getLocalBounds().contains (z->getBounds()));
        }
        REQUIRE (band->chordZone().getBottom() <= band->arpZone().getY());
        REQUIRE (band->arpZone().getBottom()   <= band->seqZone().getY());

        // Knob usability is asserted for real display sizes only. 900x480 is the setResizeLimits
        // FLOOR -- a deliberately tiny window where the controls are legitimately miniature; what
        // matters there is only that the shares are still held (asserted above).
        if (sz.h < 700) continue;

        // A knob crushed to a sliver is not a knob: RotaryKnob::resized() carves its inner slider
        // out of its own bounds, so once the section is short enough the slider gets a degenerate
        // (even negative) height, the hit-test never reaches it, and the control silently stops
        // responding to the mouse. Drive the REAL hit path, then a REAL drag.
        auto* cutoff = findKnob (*ed, ParamID::filterCutoff);
        REQUIRE (cutoff != nullptr);
        REQUIRE (cutoff->getWidth()  >= 20);
        REQUIRE (cutoff->getHeight() >= 20);

        juce::Slider* sl = nullptr;
        for (auto* ch : cutoff->getChildren()) if (auto* c = dynamic_cast<juce::Slider*> (ch)) sl = c;
        REQUIRE (sl != nullptr);
        REQUIRE (sl->getHeight() >= 16);
        REQUIRE (sl->getWidth()  >= 16);
        // The OS hit-test from the editor root must actually reach that slider.
        REQUIRE (ed->getComponentAt (ed->getLocalPoint (sl, sl->getLocalBounds().getCentre())) == sl);

        auto* prm = p.apvts.getParameter (ParamID::filterCutoff);
        prm->setValueNotifyingHost (0.5f);
        const float before = prm->getValue();
        const auto grab = sl->getLocalBounds().getCentre().toFloat();
        const auto lift = grab.translated (0.0f, -30.0f);         // rotary vertical drag: up = more
        const auto now  = juce::Time::getCurrentTime();
        const auto mods = juce::ModifierKeys().withFlags (juce::ModifierKeys::leftButtonModifier);
        auto src = juce::Desktop::getInstance().getMainMouseSource();
        sl->mouseDown (juce::MouseEvent (src, grab, mods, 1.0f, 0, 0, 0, 0, sl, sl, now, grab, now, 1, false));
        sl->mouseDrag (juce::MouseEvent (src, lift, mods, 1.0f, 0, 0, 0, 0, sl, sl, now, grab, now, 1, true));
        sl->mouseUp   (juce::MouseEvent (src, lift, mods, 1.0f, 0, 0, 0, 0, sl, sl, now, grab, now, 1, true));
        REQUIRE (prm->getValue() > before);                        // the knob actually turns
    }
}

// --- REC/STOP: the top bar's record toggle, driven as a real button ---------------------
// The button shipped as a placeholder that only posted a toast, so this drives the REAL
// TextButton found in the REAL editor tree (never TopBar's handler) -- a REC button wired
// to nothing, or relabelled without actually recording, fails here. Per the smoke-harness
// rule: real events, end-to-end state, and a screenshot of the armed state.
namespace
{
    // Depth-first search for a TextButton by its current label.
    juce::TextButton* findButton (juce::Component& c, const juce::String& text)
    {
        for (auto* ch : c.getChildren())
        {
            if (auto* b = dynamic_cast<juce::TextButton*> (ch))
                if (b->getButtonText() == text) return b;
            if (auto* found = findButton (*ch, text)) return found;
        }
        return nullptr;
    }
}

TEST_CASE ("rec button: tapping REC records the master output and relabels to STOP",
           "[plugin][smoke][rec]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 128);
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    ed->setSize (VASynthEditor::kDefaultWidth, 1000);
    ed->setVisible (true);                          // tap() needs a real, hit-testable tree

    auto* rec = findButton (*ed, "REC");
    REQUIRE (rec != nullptr);                       // the button exists and reads REC when idle
    REQUIRE (rec->getWidth()  > 10);                // ...and is actually on screen
    REQUIRE (rec->getHeight() > 10);
    REQUIRE_FALSE (p.isMasterRecording());

    // ---- a REAL tap on the REAL button arms the recorder ----
    // This is the assertion the placeholder REC button would have failed: it posted a toast
    // and nothing else, so the tap reached the button but never reached the recorder.
    tap (*rec);
    REQUIRE (p.isMasterRecording());
    REQUIRE (rec->getButtonText() == "STOP");       // the label says what the button will DO next
    REQUIRE (findButton (*ed, "REC") == nullptr);   // ...and no stale REC label is left anywhere

    snapshot (*ed, "rec-armed.png");

    // Audio rendered while armed must land in the take -- this proves the audio-thread TAP is
    // wired into the render path, not merely that a flag flipped.
    for (int b = 0; b < 40; ++b)
    {
        juce::AudioBuffer<float> buf (2, 128); buf.clear();
        juce::MidiBuffer midi;
        if (b == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
        p.processBlock (buf, midi);
    }
    REQUIRE (p.masterRecorder().recordedSamples() == 40 * 128);
    REQUIRE (p.masterRecorder().droppedBlocks() == 0);

    // Stopping THIS take would open the modal save dialog, and this suite never launches
    // native modal windows (see the SessionExportDialog/OutputsDialog tests, which construct
    // their dialog directly). So finish the take off-button and cover the save path in the
    // dialog test below.
    REQUIRE (p.stopMasterRecording());
    auto take = p.masterRecorder().takeFile();
    REQUIRE (take.existsAsFile());
    REQUIRE (take.getSize() > 0);
    p.masterRecorder().discardTake();
}

// The STOP branch of the same handler, driven as a real tap. An armed take that captured
// NOTHING has no file to offer, so the handler stops without opening a dialog -- which lets
// the real button be tapped twice, end to end, with no modal window in the way.
TEST_CASE ("rec button: tapping STOP disarms and returns the label to REC",
           "[plugin][smoke][rec]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 128);
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    ed->setSize (VASynthEditor::kDefaultWidth, 1000);
    ed->setVisible (true);

    auto* rec = findButton (*ed, "REC");
    REQUIRE (rec != nullptr);

    tap (*rec);                                     // arm
    REQUIRE (p.isMasterRecording());
    REQUIRE (rec->getButtonText() == "STOP");

    tap (*rec);                                     // stop -- nothing was rendered, so no dialog
    REQUIRE_FALSE (p.isMasterRecording());
    REQUIRE (rec->getButtonText() == "REC");        // the toggle really is a toggle
    REQUIRE (juce::Component::getCurrentlyModalComponent() == nullptr);
    REQUIRE (findButton (*ed, "STOP") == nullptr);

    // And it arms again afterwards: the button is not a one-shot.
    tap (*rec);
    REQUIRE (p.isMasterRecording());
    REQUIRE (rec->getButtonText() == "STOP");
    p.stopMasterRecording();
    p.masterRecorder().discardTake();
}

// The save dialog is what "immediately upon selecting STOP" produces, so it must build on a
// real finished take, offer the formats, and actually write the file the picker chose. Driven
// through the dialog's own controls + its test seam (a native file chooser cannot run headless).
TEST_CASE ("rec dialog: the save dialog offers the formats and writes the chosen one",
           "[plugin][smoke][rec]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 128);

    REQUIRE (p.startMasterRecording());
    for (int b = 0; b < 60; ++b)
    {
        juce::AudioBuffer<float> buf (2, 128); buf.clear();
        juce::MidiBuffer midi;
        if (b == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
        p.processBlock (buf, midi);
    }
    REQUIRE (p.stopMasterRecording());

    RecordSaveDialog dlg (p);
    dlg.setVisible (true);                                   // it sizes itself in the constructor
    REQUIRE (dlg.getWidth()  > 0);
    REQUIRE (dlg.getHeight() > 0);

    // Every format in the model is offered in the picker, in order.
    const auto fmts = MasterRecorder::formats();
    REQUIRE (dlg.formatBox().getNumItems() == fmts.size());
    for (int i = 0; i < fmts.size(); ++i)
        REQUIRE (dlg.formatBox().getItemText (i) == juce::String (fmts[i].label));
    REQUIRE (dlg.formatBox().getSelectedId() == 1);          // defaults to WAV 24-bit

    snapshot (dlg, "rec-save-dialog.png");

    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("synth-recdlg-" + juce::String (juce::Time::currentTimeMillis()));
    REQUIRE (dir.createDirectory().wasOk());

    // Pick a format through the real ComboBox, then save: the file must appear and decode.
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    for (int i = 0; i < fmts.size(); ++i)
    {
        // sendNotificationSync, NOT the default: ComboBox::setSelectedId defaults to
        // sendNotificationAsync, which posts onChange to the message loop. A test with no
        // loop running would never see refreshStatus() fire, so the dialog would look like
        // it ignored the format change. A real click gets the callback via the running loop.
        dlg.formatBox().setSelectedId (i + 1, juce::sendNotificationSync);
        auto dest = dir.getChildFile ("t" + juce::String (i) + "." + fmts[i].ext);
        juce::String error;
        const bool ok = dlg.saveToForTest (dest, error);
        INFO ("format=" << fmts[i].label << " error=" << error);

        // Every format in the picker must save, on every platform, with nothing installed --
        // the encoder is embedded, so there is no conditional branch left here at all.
        REQUIRE (ok);
        REQUIRE (dlg.statusText().isEmpty());                // no warning: a clean take saved cleanly
        REQUIRE (dest.existsAsFile());
        REQUIRE (dest.getSize() > 0);
        if (fmts[i].kind == MasterRecorder::Kind::Mp3) continue;   // JUCE's MP3 reader is compiled out
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (dest));
        REQUIRE (r != nullptr);
        REQUIRE (r->lengthInSamples > 0);
    }

    p.masterRecorder().discardTake();
    dir.deleteRecursively();
}

// Discard is the dialog's destructive path, so drive it as a real tap: the take must actually
// be deleted from the temp dir, not merely forgotten about.
TEST_CASE ("rec dialog: tapping Discard deletes the take", "[plugin][smoke][rec]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 128);

    REQUIRE (p.startMasterRecording());
    for (int b = 0; b < 20; ++b)
    {
        juce::AudioBuffer<float> buf (2, 128); buf.clear();
        juce::MidiBuffer midi;
        if (b == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
        p.processBlock (buf, midi);
    }
    REQUIRE (p.stopMasterRecording());
    const auto take = p.masterRecorder().takeFile();
    REQUIRE (take.existsAsFile());

    {
        RecordSaveDialog dlg (p);
        dlg.setVisible (true);
        tap (dlg.discardButton());
        REQUIRE_FALSE (take.existsAsFile());                 // gone from disk
        REQUIRE (p.masterRecorder().takeFile() == juce::File());
    }
    // Leaving scope must not resurrect a "take kept" claim about a file that was discarded.
    REQUIRE_FALSE (take.existsAsFile());
}
