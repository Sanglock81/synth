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
#include "UI/ModMatrixPanel.h"
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
}
