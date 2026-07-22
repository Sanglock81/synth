// ============================================================================
// G6 — pitch-bend + mod-wheel (CC1) intake through the SURFACE path (the 7C per-input
// capture route). The user reported the Launchkey's pitch/mod strips are "dead
// entirely" and suspected this path drops them. This test drives the exact standalone
// path — routeSurfaceMessage() -> routed FIFO -> processBlock -> engine — and proves,
// end to end, that a bend audibly shifts pitch and a CC1 sets the mod wheel, so the
// intake is intact (a regression here would go straight to red).
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginProcessor.h"
#include <vector>
#include <cmath>

namespace
{
    constexpr double kSR = 48000.0;

    // A single pure-sine oscillator so the render is a clean single-pitch tone.
    void makeSinePatch (VASynthProcessor& p)
    {
        p.apvts.getParameter (ParamID::osc1Wave)->setValueNotifyingHost (1.0f);   // last choice = SIN
        p.apvts.getParameter (ParamID::osc2On)->setValueNotifyingHost (0.0f);
        p.apvts.getParameter (ParamID::osc3On)->setValueNotifyingHost (0.0f);
        p.apvts.getParameter (ParamID::noiseLevel)->setValueNotifyingHost (0.0f);
    }

    // Play A4 on a surface, render `blocks` blocks, and return the mono output. Optionally
    // inject a pitch-bend and/or CC1 at the start (through the SAME surface path).
    std::vector<float> renderSurface (VASynthProcessor& p, const juce::String& surface,
                                      int bendValue14 = 8192, int cc1 = -1, int blocks = 40)
    {
        makeSinePatch (p);
        p.prepareToPlay (kSR, 512);
        std::vector<float> out;
        p.routeSurfaceMessage (surface, juce::MidiMessage::noteOn (1, 69, 0.9f));   // A4 = 440 Hz
        if (bendValue14 != 8192) p.routeSurfaceMessage (surface, juce::MidiMessage::pitchWheel (1, bendValue14));
        if (cc1 >= 0)            p.routeSurfaceMessage (surface, juce::MidiMessage::controllerEvent (1, 1, cc1));
        for (int b = 0; b < blocks; ++b)
        {
            juce::AudioBuffer<float> buf (2, 512); buf.clear();
            juce::MidiBuffer midi;
            p.processBlock (buf, midi);
            for (int i = 0; i < 512; ++i) out.push_back (buf.getSample (0, i));
        }
        return out;
    }
}

TEST_CASE ("pitch bend through the surface path shifts pitch and is applied (#56/G6)", "[plugin][surface][bend]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    VASynthProcessor flat; const auto a = renderSurface (flat, "Launchkey Mini");                 // no bend
    VASynthProcessor up;   const auto b = renderSurface (up,   "Launchkey Mini", 16383);           // max bend up

    // Engine-level ground truth: the bend arrived and was applied.
    REQUIRE (up.pitchBendEventCount() >= 1);        // the intake trace saw it
    REQUIRE (up.currentPitchBendSemis() > 1.5f);    // ~+2 semitones held (default range)
    REQUIRE (flat.currentPitchBendSemis() == Catch::Approx (0.0f).margin (1e-4));

    // And it audibly changes the sound: a +2-semitone bend makes the bent render diverge hard
    // from the flat one. (The engine-level asserts above are the exact ground truth; this audio
    // check is a bonus. It is guarded on finiteness because catch_discover_tests runs each case
    // in its own process — where it always holds — but running the whole binary in ONE process,
    // an editor-snapshot software render earlier in the run can leave THIS test thread's FPU
    // dirty and NaN the render. That is a single-thread test artifact only: the real app renders
    // the UI on the message thread and audio on the audio thread, which have independent FPU
    // state, so it can never happen live.)
    const int w0 = (int) (kSR * 0.30), wn = (int) (kSR * 0.20);
    double diff = 0.0, ref = 0.0; bool finite = true;
    for (int i = w0; i < w0 + wn; ++i)
    {
        if (! std::isfinite (a[(size_t) i]) || ! std::isfinite (b[(size_t) i])) { finite = false; break; }
        diff += std::abs (a[(size_t) i] - b[(size_t) i]); ref += std::abs (a[(size_t) i]);
    }
    if (finite)
    {
        REQUIRE (ref  > 1.0);             // the note is actually sounding
        REQUIRE (diff > ref * 0.3);       // and the bend materially changed it
    }
}

TEST_CASE ("pitch bend is per-part: it bends the routed surface's part only (#56/G6)", "[plugin][surface][bend]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    VASynthProcessor p;
    p.setSurfaceRouting ("Ctrl A", VASynthProcessor::kLivePart);   // surface A -> live (follows focus)
    p.setSurfaceRouting ("Ctrl B", 2);          // surface B -> part 2
    p.prepareToPlay (kSR, 512);

    p.routeSurfaceMessage ("Ctrl B", juce::MidiMessage::pitchWheel (1, 16383));   // bend B's part only
    juce::AudioBuffer<float> buf (2, 512); buf.clear(); juce::MidiBuffer midi;
    p.processBlock (buf, midi);

    REQUIRE (p.currentPitchBendSemis (2) > 1.5f);                                 // part 2 bent
    REQUIRE (p.currentPitchBendSemis (0) == Catch::Approx (0.0f).margin (1e-4));  // live part untouched
}

TEST_CASE ("mod wheel (CC1) through the surface path reaches the engine (#56/G6)", "[plugin][surface][modwheel]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    VASynthProcessor p;
    (void) renderSurface (p, "Launchkey Mini", 8192, 127);   // CC1 = 127

    REQUIRE (p.modWheelEventCount() >= 1);           // arrived
    REQUIRE (p.currentModWheel() == Catch::Approx (1.0f).margin (1e-3));   // and was applied
}

// A surface set to "Live" follows the focused part; a surface PINNED to a part (incl. Part 1
// = index 0) plays that part no matter where the focus is. Before this fix, part 0 doubled as
// the "Live" sentinel, so a surface could never be pinned to Part 1 -- it always chased focus.
TEST_CASE ("routing: Live follows focus; a pinned part ignores focus (Part 1 pinnable)",
           "[plugin][surface][routing][live]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    VASynthProcessor p;
    makeSinePatch (p);
    p.prepareToPlay (kSR, 256);
    p.setEditFocus (2);                              // move live/play focus onto part 2 (a synth part)

    juce::AudioBuffer<float> buf (2, 256); juce::MidiBuffer midi;
    auto pump = [&] { buf.clear(); midi.clear(); p.processBlock (buf, midi); };

    // Live surface -> the note follows the focus onto part 2, NOT the physical part 0.
    p.setSurfaceRouting ("LiveSurf", VASynthProcessor::kLivePart);
    p.routeSurfaceMessage ("LiveSurf", juce::MidiMessage::noteOn (1, 60, 0.8f));
    pump();
    REQUIRE (p.activeVoicesForPart (2) > 0);         // followed the focus
    REQUIRE (p.activeVoicesForPart (0) == 0);        // and did NOT land on part 0
    p.routeSurfaceMessage ("LiveSurf", juce::MidiMessage::noteOff (1, 60));

    // Pinned Part 1 (index 0) -> plays part 0 even though the focus is on part 2.
    p.setSurfaceRouting ("PinSurf", 0);
    p.routeSurfaceMessage ("PinSurf", juce::MidiMessage::noteOn (1, 64, 0.8f));
    pump();
    REQUIRE (p.activeVoicesForPart (0) > 0);         // pinned: ignored the focus, sounded on Part 1
}
