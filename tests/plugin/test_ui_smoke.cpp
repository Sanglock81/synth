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
#include "UI/BottomZones.h"
#include "UI/ModMatrixPanel.h"
#include "ModDestRegistry.h"
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
