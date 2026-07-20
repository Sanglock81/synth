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

TEST_CASE ("scene: disarming REC while a launch is pending lets it proceed at the next quantum", "[plugin][scene][j3][escape]")
{
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    setVal (p, ParamID::tempo, 240.0f);
    setVal (p, ParamID::sceneQuant, 0.0f);           // 1-bar quantum
    p.apvts.getParameter (ParamID::loopBars)->setValueNotifyingHost (
        p.apvts.getParameter (ParamID::loopBars)->convertTo0to1 (1.0f));   // lane 0 = 2 bars
    Driver d (p); d.run (10);

    set01 (p, ParamID::loopRec, 1.0f);               // roll a take
    int guard = 0;
    while (p.loopRecDisplayState (0) != 2 && guard++ < 4000) d.run (1);
    REQUIRE (p.loopRecDisplayState (0) == 2);

    p.launchScene (1);                               // pending, deferred while recording
    d.run (200);
    REQUIRE (p.activeScene() == 0);

    set01 (p, ParamID::loopRec, 0.0f);               // DISARM REC -> the pending press was intent: proceed
    guard = 0;
    while (p.activeScene() != 1 && guard++ < 3000) d.run (1);
    REQUIRE (p.activeScene() == 1);                  // engaged at the next quantum after the take stopped
}

TEST_CASE ("scene: edits during a pending switch write to the OUTGOING scene until the flip", "[plugin][scene][j3][edge]")
{
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    setVal (p, ParamID::tempo, 120.0f);
    setVal (p, ParamID::sceneQuant, 0.0f);           // 1-bar quantum
    Driver d (p); d.run (10);

    p.launchScene (1);                               // arm a switch to scene 1
    d.run (50);                                      // still pending, mid first bar
    REQUIRE (p.activeScene() == 0);
    p.setSeqCell (6, 6, 1);                          // a non-recording edit WHILE pending -> outgoing (0)

    d.run (kBlocksPerBar + 5);                        // the switch flips to scene 1
    REQUIRE (p.activeScene() == 1);
    REQUIRE (p.getSeqCell (6, 6) == 0);              // the edit did NOT land in the incoming scene

    p.launchScene (0); d.run (kBlocksPerBar + 5);    // back to the scene that was live when we edited
    REQUIRE (p.activeScene() == 0);
    REQUIRE (p.getSeqCell (6, 6) == 1);              // it was captured into the outgoing scene
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

TEST_CASE ("scene: a switch waits for an in-progress recording to finish (all parts complete)", "[plugin][scene][j3][edge]")
{
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    setVal (p, ParamID::tempo, 240.0f);              // bar = 48000/128 = 375 blocks; keeps the take short
    setVal (p, ParamID::sceneQuant, 0.0f);           // even at the 1-bar quantum, a take defers the switch
    p.apvts.getParameter (ParamID::loopBars)->setValueNotifyingHost (
        p.apvts.getParameter (ParamID::loopBars)->convertTo0to1 (1.0f));   // lane 0 = 2 bars
    Driver d (p); d.run (10);

    // Roll a 2-bar take on lane 0.
    set01 (p, ParamID::loopRec, 1.0f);
    int guard = 0;
    while (p.loopRecDisplayState (0) != 2 && guard++ < 4000) d.run (1);
    REQUIRE (p.loopRecDisplayState (0) == 2);
    p.routeNoteOn (60, 0.9f, 0); d.run (4); p.routeNoteOff (60, 0);
    REQUIRE (p.loopLaneEventCount (0) > 0);

    // Launch scene 1 mid-take: the switch must NOT happen while the take is still recording.
    p.launchScene (1);
    d.run (200);                                      // still recording -> no switch yet
    REQUIRE (p.loopRecDisplayState (0) == 2);
    REQUIRE (p.activeScene() == 0);

    // Once the take finishes (in scene 0), the deferred switch engages.
    guard = 0;
    while (p.activeScene() != 1 && guard++ < 6000) d.run (1);
    REQUIRE (p.activeScene() == 1);
    // The take stayed in the scene it was recorded in (0), not the one we switched to.
    p.launchScene (0); d.run (3 * kBlocksPerBar);
    REQUIRE (p.activeScene() == 0);
    REQUIRE (p.loopLaneHasContent (0));
}

TEST_CASE ("scene: AUDIO loops are isolated per scene (record -> switch away -> switch back)", "[plugin][scene][j3][audio]")
{
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    p.loadInitPreset();
    setVal (p, ParamID::tempo, 240.0f);              // short bars
    setVal (p, ParamID::sceneQuant, 0.0f);           // 1-bar quantum
    p.apvts.getParameter (ParamID::loopBars)->setValueNotifyingHost (
        p.apvts.getParameter (ParamID::loopBars)->convertTo0to1 (0.0f));   // lane 0 = 1 bar
    set01 (p, ParamID::loopMode, 1.0f);              // lane 0 AUDIO mode
    Driver d (p); d.run (10);

    // Record an AUDIO take on lane 0: hold a note (produces capturable audio) while a one-shot rolls.
    p.routeNoteOn (60, 0.9f, 0);
    set01 (p, ParamID::loopRec, 1.0f);
    int guard = 0;
    while (p.loopRecDisplayState (0) != 2 && guard++ < 6000) d.run (1);
    guard = 0;
    while (p.loopRecDisplayState (0) == 2 && guard++ < 6000) d.run (1);   // one-shot completes -> auto-play
    p.routeNoteOff (60, 0);
    REQUIRE (p.loopAudioHasContent (0));             // scene 0 captured audio

    // Switch to the empty scene 1: its (empty) audio replaces the live ring.
    p.launchScene (1);
    guard = 0; while (p.activeScene() != 1 && guard++ < 6000) d.run (1);
    p.pumpSceneWorkForTest();                        // run the deferred audio swap (message-thread work)
    d.run (2);
    REQUIRE_FALSE (p.loopAudioHasContent (0));       // scene 1 is a blank canvas

    // Switch back to scene 0: its audio must return (not lost, not shared).
    p.launchScene (0);
    guard = 0; while (p.activeScene() != 0 && guard++ < 6000) d.run (1);
    p.pumpSceneWorkForTest();
    d.run (2);
    REQUIRE (p.loopAudioHasContent (0));
}

TEST_CASE ("scene: a newly-activated scene starts from the beginning (loop rewinds)", "[plugin][scene][j3]")
{
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    setVal (p, ParamID::tempo, 120.0f);
    setVal (p, ParamID::sceneQuant, 0.0f);           // 1-bar quantum for a deterministic engage
    Driver d (p);
    d.run (300);                                      // advance well into a bar so the phase is non-zero
    REQUIRE (p.loopPlayhead (0) > 0.1f);              // mid-loop before the switch

    p.launchScene (1);
    int guard = 0;
    while (p.activeScene() != 1 && guard++ < 2000) d.run (1);
    REQUIRE (p.activeScene() == 1);
    d.run (1);
    REQUIRE (p.loopPlayhead (0) < 0.02f);            // the new scene begins at its downbeat, not mid-loop
}
