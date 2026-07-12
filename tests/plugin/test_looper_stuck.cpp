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
