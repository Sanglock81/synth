// Synth — Copyright © 2026 John L Farmer. Licensed under AGPLv3; see LICENSE.
// ============================================================================
// Looper stuck-note regression. The MIDI loop can hold a note-on with no matching
// note-off (a note recorded held THROUGH the loop, or playback stopped mid-note); it
// would re-fire forever and hang when playback stops. The processor tracks the notes
// the loop turned on and flushes them when playback stops or the loop clears.
// Invariant: after stopping the recorder, the dry output is silent.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "PluginProcessor.h"
#include <cmath>

namespace
{
    void s01 (VASynthProcessor& p, const char* id, float v) { p.apvts.getParameter (id)->setValueNotifyingHost (v); }
    void setVal (VASynthProcessor& p, const char* id, float v)
    { p.apvts.getParameter (id)->setValueNotifyingHost (p.apvts.getParameter (id)->convertTo0to1 (v)); }

    struct Rig
    {
        VASynthProcessor p; juce::AudioBuffer<float> buf; juce::MidiBuffer m;
        Rig() : buf (2, 128) { p.prepareToPlay (48000.0, 128); p.loadInitPreset(); setVal (p, ParamID::ampRelease, 0.02f); }
        void pump (int n) { for (int b = 0; b < n; ++b) { buf.clear(); p.processBlock (buf, m); } }
        float tailPeak (int settle = 300, int measure = 150)
        {
            pump (settle);
            float pk = 0.0f;
            for (int b = 0; b < measure; ++b)
            { buf.clear(); p.processBlock (buf, m);
              for (int i = 0; i < buf.getNumSamples(); ++i) pk = std::max ({ pk, std::abs (buf.getSample (0, i)), std::abs (buf.getSample (1, i)) }); }
            return pk;
        }
    };
}

TEST_CASE ("no hang: note held through record then playback stops releases it", "[plugin][looper][stuck]")
{
    // The reported recorder stuck note: a note held through the loop records an ON with no
    // OFF, re-fires each cycle, and hangs when playback stops.
    Rig r; setVal (r.p, ParamID::tempo, 220.0f);
    s01 (r.p, ParamID::loopRec, 1.0f); s01 (r.p, ParamID::loopPlay, 1.0f);
    r.pump (2);
    r.p.routeNoteOn (60, 0.9f, 0);               // held; only the ON is recorded
    r.pump (300);
    s01 (r.p, ParamID::loopRec, 0.0f);           // stop recording (note still held)
    r.p.routeNoteOff (60, 0);                    // release the live key (not recorded)
    r.pump (300);                                // loop re-fires 60 with no OFF
    REQUIRE (r.p.loopLaneHasContent (0));        // the loop really recorded something
    s01 (r.p, ParamID::loopPlay, 0.0f);          // stop playback
    REQUIRE (r.tailPeak() < 1.0e-4f);            // ...and the note is released, not stuck
}

TEST_CASE ("no hang: CLEAR while a loop is playing releases sounding notes", "[plugin][looper][stuck]")
{
    Rig r; setVal (r.p, ParamID::tempo, 220.0f);
    s01 (r.p, ParamID::loopRec, 1.0f); s01 (r.p, ParamID::loopPlay, 1.0f);
    r.pump (2);
    r.p.routeNoteOn (60, 0.9f, 0);               // held through the loop
    r.pump (300);
    s01 (r.p, ParamID::loopRec, 0.0f);
    r.p.routeNoteOff (60, 0);
    r.pump (200);
    r.p.clearLoops();                            // CLEAR mid-playback
    s01 (r.p, ParamID::loopPlay, 0.0f);
    REQUIRE (r.tailPeak() < 1.0e-4f);
}

TEST_CASE ("looper: a recorded chord drives the arp on playback, then stops clean", "[plugin][looper][arp][stuck]")
{
    // A loop records the STRUCK keys (pre-arp). With the arp on the play-focus part, its looped notes
    // must feed the arp so the loop arpeggiates — and stopping playback must leave NO stuck arpeggio
    // (the flush releases the loop's held notes THROUGH the arp).
    Rig r; setVal (r.p, ParamID::tempo, 220.0f);
    s01 (r.p, ParamID::arpOn, 1.0f);                                     // arp on (play-focus part 0)
    s01 (r.p, ParamID::loopRec, 1.0f); s01 (r.p, ParamID::loopPlay, 1.0f);
    r.pump (2);                                                          // record engages at the downbeat
    r.p.routeNoteOn (60, 0.9f, 0); r.p.routeNoteOn (64, 0.9f, 0); r.p.routeNoteOn (67, 0.9f, 0);   // held chord
    r.pump (300);
    s01 (r.p, ParamID::loopRec, 0.0f);                                  // stop record (chord held -> closeDangling closes it)
    r.p.routeNoteOff (60, 0); r.p.routeNoteOff (64, 0); r.p.routeNoteOff (67, 0);   // release the LIVE keys
    r.pump (30);

    REQUIRE (r.p.loopLaneHasContent (0));
    // Playback: the looped chord drives the arp -> a running step playhead + actual sound. If the
    // looped notes reached NOTHING (routing broken) the arp would stay idle (-1) and it'd be silent.
    bool arpRan = false; float pk = 0.0f;
    for (int b = 0; b < 500; ++b)
    {
        r.buf.clear(); r.p.processBlock (r.buf, r.m);
        if (r.p.arpDisplayStep() >= 0) arpRan = true;
        for (int i = 0; i < r.buf.getNumSamples(); ++i) pk = std::max (pk, std::abs (r.buf.getSample (0, i)));
    }
    REQUIRE (arpRan);                                                   // the loop drives the arp
    REQUIRE (pk > 1.0e-3f);                                             // ...and it sounds

    s01 (r.p, ParamID::loopPlay, 0.0f);                                // stop playback
    REQUIRE (r.tailPeak() < 1.0e-4f);                                  // no stuck arpeggio
    REQUIRE (r.p.arpDisplayStep() == -1);                              // arp idled
}

TEST_CASE ("looper: playback stays bounded over many loops (no accumulating triggers, #138)", "[plugin][looper][stuck]")
{
    // #138 regression. A recorded MIDI loop, played for many cycles, must NOT accumulate voices:
    // every recorded note-on has a matching note-off (closeDangling pairs one for a note held
    // across the one-shot record stop), so playback re-fires a BALANCED clip each pass and the
    // active-voice count stays bounded by the clip's own polyphony (plus short release tails) —
    // it must never climb toward the voice cap. If the accumulating-trigger bug returns, maxV
    // grows every loop and trips these ceilings.
    auto maxVoicesOverLoops = [] (std::function<void(Rig&)> perform, int ceiling)
    {
        Rig r; setVal (r.p, ParamID::tempo, 240.0f); setVal (r.p, ParamID::ampRelease, 0.05f);
        s01 (r.p, ParamID::loopRec, 1.0f); s01 (r.p, ParamID::loopPlay, 1.0f);
        r.pump (2);                                                   // record engages at the downbeat
        perform (r);
        int rec = 0; while (r.p.loopRecDisplayState (0) == 2 && rec < 3000) { r.pump (1); ++rec; }   // finish one pass -> plays
        REQUIRE (r.p.loopLaneHasContent (0));
        int maxV = 0;
        for (int b = 0; b < 8000; ++b)                               // ~20 loops @ 240 BPM, 1 bar
        { r.buf.clear(); r.p.processBlock (r.buf, r.m); maxV = std::max (maxV, r.p.activeVoicesForPart (0)); }
        INFO ("maxVoicesOverPlayback=" << maxV << " ceiling=" << ceiling);
        REQUIRE (maxV <= ceiling);
    };

    SECTION ("released phrase (two notes, each released inside the loop)")
        maxVoicesOverLoops ([] (Rig& r) {
            r.p.routeNoteOn (60, 0.9f, 0); r.pump (30); r.p.routeNoteOff (60, 0); r.pump (10);
            r.p.routeNoteOn (67, 0.9f, 0); r.pump (30); r.p.routeNoteOff (67, 0);
        }, 4);

    SECTION ("chord (three notes struck together, released inside the loop)")
        maxVoicesOverLoops ([] (Rig& r) {
            r.p.routeNoteOn (60, 0.9f, 0); r.p.routeNoteOn (64, 0.9f, 0); r.p.routeNoteOn (67, 0.9f, 0);
            r.pump (40); r.p.routeNoteOff (60, 0); r.p.routeNoteOff (64, 0); r.p.routeNoteOff (67, 0);
        }, 6);

    SECTION ("note held across the one-shot record stop (closeDangling path)")
        maxVoicesOverLoops ([] (Rig& r) {
            r.p.routeNoteOn (55, 0.9f, 0); r.pump (600); r.p.routeNoteOff (55, 0);   // released only after record auto-stops
        }, 3);
}

TEST_CASE ("no hang: switching loop MIDI->AUDIO then stopping releases the MIDI note", "[plugin][looper][stuck]")
{
    // Flip to AUDIO (the MIDI lane stops -> its held note must be released), then stop all
    // playback so the audio lane is silent too and any stranded MIDI voice would show.
    Rig r; setVal (r.p, ParamID::tempo, 220.0f);
    s01 (r.p, ParamID::loopMode, 0.0f);          // MIDI
    s01 (r.p, ParamID::loopRec, 1.0f); s01 (r.p, ParamID::loopPlay, 1.0f);
    r.pump (2);
    r.p.routeNoteOn (60, 0.9f, 0);               // held through the loop (ON, no OFF)
    r.pump (300);
    s01 (r.p, ParamID::loopRec, 0.0f);
    r.p.routeNoteOff (60, 0);
    r.pump (200);
    s01 (r.p, ParamID::loopMode, 1.0f);          // flip to AUDIO -> MIDI lane stops (flush)
    r.pump (50);
    s01 (r.p, ParamID::loopPlay, 0.0f);          // stop everything (audio lane silent too)
    REQUIRE (r.tailPeak() < 1.0e-4f);
}
