// ============================================================================
// J3 scenes. Eight arrangement snapshots (loop clips + drum pattern + per-lane transport).
// The ACTIVE scene IS the live state (direct-edit, no store): recording/pattern edits write into
// it automatically. Tap = launch, quantized to the master bar clock; long-press = copy/clear.
// Edge rule: a recording that completes after the flip lands in the NEW scene (not split).
//
// These drive the full processor and assert the audio-thread-immediate behaviour (clip + pattern
// swap + active index). The per-lane transport restore is a message-thread async (verified in the
// UI/integration layer); it is not exercised here.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "PluginProcessor.h"

namespace
{
    void setVal (VASynthProcessor& p, const char* id, float v)
    { auto* pr = p.apvts.getParameter (id); pr->setValueNotifyingHost (pr->convertTo0to1 (v)); }
    void set01 (VASynthProcessor& p, const char* id, float v01)
    { p.apvts.getParameter (id)->setValueNotifyingHost (v01); }

    struct Driver
    {
        VASynthProcessor& p; juce::AudioBuffer<float> buf;
        explicit Driver (VASynthProcessor& proc) : p (proc), buf (2, 128) {}
        void run (int blocks) { for (int b = 0; b < blocks; ++b) { buf.clear(); juce::MidiBuffer m; p.processBlock (buf, m); } }
    };
    // Blocks per bar at 120 BPM / 48 kHz: bar = 96000 samples / 128 = 750 blocks.
    constexpr int kBlocksPerBar = 750;
}

TEST_CASE ("scene: launching is quantized to the bar, not immediate", "[plugin][scene][j3]")
{
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    setVal (p, ParamID::tempo, 120.0f);
    setVal (p, ParamID::sceneQuant, 0.0f);           // 1-bar quantum
    Driver d (p);
    d.run (20);                                      // warm up so the bar edge tracker is established

    REQUIRE (p.activeScene() == 0);
    p.launchScene (1);
    REQUIRE (p.pendingSceneIndex() == 1);
    d.run (100);                                     // still mid-bar -> NOT switched yet
    REQUIRE (p.activeScene() == 0);
    REQUIRE (p.pendingSceneIndex() == 1);
    d.run (kBlocksPerBar);                            // cross a bar boundary
    REQUIRE (p.activeScene() == 1);                  // engaged
    REQUIRE (p.pendingSceneIndex() == -1);           // cleared
}

TEST_CASE ("scene: a launch does not engage on the very first block (clock priming)", "[plugin][scene][j3]")
{
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    setVal (p, ParamID::tempo, 120.0f);
    setVal (p, ParamID::sceneQuant, 0.0f);
    Driver d (p);
    p.launchScene (2);                               // armed before ANY processBlock
    d.run (1);                                       // the priming block must NOT falsely engage
    REQUIRE (p.activeScene() == 0);
    REQUIRE (p.pendingSceneIndex() == 2);
    d.run (50);
    REQUIRE (p.activeScene() == 0);                  // still mid first bar
}

TEST_CASE ("scene: launching restores that scene's drum pattern (edits stay per-scene)", "[plugin][scene][j3]")
{
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    setVal (p, ParamID::tempo, 120.0f);
    setVal (p, ParamID::sceneQuant, 0.0f);
    Driver d (p);
    d.run (10);

    p.setSeqCell (0, 0, 1);                          // scene 0: cell (0,0)
    p.copyActiveSceneTo (1); d.run (2);              // clone scene 0 -> scene 1 (both have (0,0))
    p.setSeqCell (1, 1, 1);                          // now edit live (scene 0): add cell (1,1)

    p.launchScene (1); d.run (kBlocksPerBar + 5);    // switch to scene 1 at the bar
    REQUIRE (p.activeScene() == 1);
    REQUIRE (p.getSeqCell (0, 0) == 1);              // scene 1 kept the cloned cell
    REQUIRE (p.getSeqCell (1, 1) == 0);              // but NOT the scene-0-only edit

    p.launchScene (0); d.run (kBlocksPerBar + 5);    // back to scene 0
    REQUIRE (p.activeScene() == 0);
    REQUIRE (p.getSeqCell (0, 0) == 1);
    REQUIRE (p.getSeqCell (1, 1) == 1);              // the edit is preserved in scene 0
}

TEST_CASE ("scene: an empty scene launches as a blank canvas; clear wipes", "[plugin][scene][j3]")
{
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    setVal (p, ParamID::tempo, 120.0f);
    setVal (p, ParamID::sceneQuant, 0.0f);
    Driver d (p);
    d.run (10);
    p.setSeqCell (0, 3, 1);                          // scene 0 has content
    d.run (2);
    REQUIRE (p.getSeqCell (0, 3) == 1);

    p.launchScene (5); d.run (kBlocksPerBar + 5);    // scene 5 is empty
    REQUIRE (p.activeScene() == 5);
    REQUIRE (p.getSeqCell (0, 3) == 0);              // blank canvas

    p.setSeqCell (2, 2, 1); d.run (2);               // fill scene 5
    REQUIRE (p.sceneHasContent (5));
    p.clearScene (5); d.run (2);                     // wipe the (active) scene
    REQUIRE (p.getSeqCell (2, 2) == 0);
    REQUIRE_FALSE (p.sceneHasContent (5));

    p.launchScene (0); d.run (kBlocksPerBar + 5);    // scene 0's content survived the detour
    REQUIRE (p.getSeqCell (0, 3) == 1);
}

TEST_CASE ("scene: re-tapping a pending scene cancels the switch", "[plugin][scene][j3]")
{
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    setVal (p, ParamID::tempo, 120.0f);
    setVal (p, ParamID::sceneQuant, 0.0f);
    Driver d (p); d.run (10);

    p.launchScene (3);
    REQUIRE (p.pendingSceneIndex() == 3);
    p.launchScene (3);                               // re-tap cancels
    REQUIRE (p.pendingSceneIndex() == -1);
    d.run (kBlocksPerBar + 5);
    REQUIRE (p.activeScene() == 0);                  // never switched
}

TEST_CASE ("scene edge rule: a recording crossing the flip lands in the NEW scene", "[plugin][scene][j3][edge]")
{
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    setVal (p, ParamID::tempo, 240.0f);              // bar = 48000/128 = 375 blocks; keeps the take short
    setVal (p, ParamID::sceneQuant, 0.0f);           // 1-bar launch quantum
    p.apvts.getParameter (ParamID::loopBars)->setValueNotifyingHost (
        p.apvts.getParameter (ParamID::loopBars)->convertTo0to1 (1.0f));   // lane 0 = choice index 1 == 2 bars
    Driver d (p); d.run (10);

    // Arm a 2-bar recording on lane 0 and wait for the one-shot to actually ENGAGE (at the lane's
    // 2-bar downbeat). loopRecDisplayState: 0 idle, 1 armed, 2 recording.
    set01 (p, ParamID::loopRec, 1.0f);
    int guard = 0;
    while (p.loopRecDisplayState (0) != 2 && guard++ < 4000) d.run (1);
    REQUIRE (p.loopRecDisplayState (0) == 2);         // take is rolling
    p.routeNoteOn (60, 0.9f, 0); d.run (4); p.routeNoteOff (60, 0);   // a note early in the take
    INFO ("after note: events=" << p.loopLaneEventCount (0) << " recState=" << p.loopRecDisplayState (0));
    REQUIRE (p.loopLaneEventCount (0) > 0);            // the note was captured into the take

    // Launch scene 1: with a 1-bar quantum inside a 2-bar take, the flip lands MID-take.
    p.launchScene (1);
    guard = 0;
    while (p.activeScene() != 1 && guard++ < 4000) d.run (1);
    REQUIRE (p.activeScene() == 1);                   // switched during the recording
    INFO ("after flip: events=" << p.loopLaneEventCount (0) << " recState=" << p.loopRecDisplayState (0));

    // Let the take finish (one-shot auto-stops at the 2-bar boundary, after the flip).
    guard = 0;
    while (p.loopRecDisplayState (0) == 2 && guard++ < 4000) d.run (1);
    d.run (5);
    INFO ("after complete: events=" << p.loopLaneEventCount (0));
    // The completed take is in the NEW scene (1), and did NOT split back into scene 0.
    REQUIRE (p.loopLaneHasContent (0));
    p.launchScene (0); d.run (2 * kBlocksPerBar);     // (kBlocksPerBar computed @120; ample at 240)
    REQUIRE (p.activeScene() == 0);
    REQUIRE_FALSE (p.loopLaneHasContent (0));          // scene 0 never received the crossing take
}
