// ============================================================================
// Bug B (step zero for Phase 7): the standalone's ability to PLAY must survive
// the app lifecycle — startup, Audio/MIDI settings open/close, preset switch, and
// window focus loss/regain — for BOTH the QWERTY keyboard and any MIDI controller.
//
// Two mechanisms are tested here:
//   * QWERTY keyboard-focus RECLAIM decision (qwertyShouldReclaimFocus) — the pure
//     predicate the editor's watchdog uses; every lifecycle case is a row below.
//   * MIDI-input RE-ASSERT set logic (inputsNeedingEnable) — which present inputs
//     must be (re)enabled so a settings-dialog toggle can't silently kill a surface.
// Plus: switching presets must not break the note-render plumbing.
//
// True focus grabbing / device enabling is editor+device-manager integration
// (standalone only) and is on the hands-on checklist; the decision logic that
// drives it is pinned deterministically here.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Standalone/AudioDeviceCuration.h"
#include <cmath>

// ---- QWERTY focus-reclaim decision -----------------------------------------
TEST_CASE ("Bug B: QWERTY focus is reclaimed exactly when it is safe", "[plugin][bugB][focus]")
{
    // args: (standalone, showing, windowActive, weHaveFocus, modalOpen, textFieldFocused)

    // The four lifecycle scenarios where play must resume — window active, we lost
    // focus to a thief, nothing modal: RECLAIM.
    SECTION ("startup / settings-close / preset-load / alt-tab-back → reclaim")
    {
        REQUIRE (qwertyShouldReclaimFocus (true, true, true, false, false, false));
    }

    // We already hold focus: do nothing (no thrash / no re-grab storms).
    SECTION ("already have focus → no reclaim")
    {
        REQUIRE_FALSE (qwertyShouldReclaimFocus (true, true, true, true, false, false));
    }

    // Window not active (user Alt-Tabbed AWAY): never fight another app for focus.
    SECTION ("window inactive → no reclaim")
    {
        REQUIRE_FALSE (qwertyShouldReclaimFocus (true, true, false, false, false, false));
    }

    // A modal dialog is up (Save / INPUTS name entry): don't steal the user's typing.
    SECTION ("modal dialog open → no reclaim")
    {
        REQUIRE_FALSE (qwertyShouldReclaimFocus (true, true, true, false, true, false));
    }

    // A text field has focus: same — don't hijack typing.
    SECTION ("text field focused → no reclaim")
    {
        REQUIRE_FALSE (qwertyShouldReclaimFocus (true, true, true, false, false, true));
    }

    // Not the standalone (plugin in a host): the host owns the keyboard.
    SECTION ("plugin (not standalone) → no reclaim")
    {
        REQUIRE_FALSE (qwertyShouldReclaimFocus (false, true, true, false, false, false));
    }

    // Off screen: nothing to reclaim.
    SECTION ("not showing → no reclaim")
    {
        REQUIRE_FALSE (qwertyShouldReclaimFocus (true, false, true, false, false, false));
    }
}

// ---- MIDI input re-assert set logic ----------------------------------------
TEST_CASE ("Bug B: every present MIDI input that isn't enabled gets re-enabled", "[plugin][bugB][midi]")
{
    using AudioDeviceCuration::inputsNeedingEnable;

    // Two controllers present; one was silently disabled (e.g. by a settings toggle).
    juce::StringArray present { "korg-b2-id", "launchkey-id" };
    juce::StringArray enabled { "korg-b2-id" };
    auto need = inputsNeedingEnable (present, enabled);
    REQUIRE (need.size() == 1);
    REQUIRE (need.contains ("launchkey-id"));   // the dropped surface is re-enabled

    // Steady state: all enabled -> nothing to do (idempotent, no re-enable storm).
    REQUIRE (inputsNeedingEnable (present, present).isEmpty());

    // Fresh: nothing enabled yet -> enable them all (startup / hot-plug).
    REQUIRE (inputsNeedingEnable (present, {}).size() == 2);

    // No devices -> nothing.
    REQUIRE (inputsNeedingEnable ({}, {}).isEmpty());
}

// ---- preset switch must not break the note plumbing ------------------------
TEST_CASE ("Bug B: switching presets leaves the play path intact", "[plugin][bugB][preset]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 256);

    auto rmsOverBlocks = [&] (int nBlocks)
    {
        double acc = 0.0; int n = 0;
        for (int b = 0; b < nBlocks; ++b)
        {
            juce::AudioBuffer<float> buf (2, 256); buf.clear();
            juce::MidiBuffer midi;
            p.processBlock (buf, midi);
            const float* ch = buf.getReadPointer (0);
            for (int i = 0; i < 256; ++i) { acc += double (ch[i]) * ch[i]; ++n; }
        }
        return std::sqrt (acc / n);
    };

    // Cycle Init -> a factory preset -> Init, and after each, a played note sounds.
    for (const char* preset : { "Init", "Fat Saw Bass", "Init" })
    {
        if (juce::String (preset) == "Init") p.loadInitPreset();
        else                                 p.loadFactoryPreset (preset);

        p.qwertyKeyboardState.noteOn (1, 60, 0.8f);
        REQUIRE (rmsOverBlocks (16) > 0.005);           // it still plays after the switch
        p.qwertyKeyboardState.noteOff (1, 60, 0.0f);
        rmsOverBlocks (48);                              // let it release before the next round
    }
}
