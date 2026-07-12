// ============================================================================
// Stuck-note regression: transitions between the note GENERATORS (arp / sequencer /
// looper) and their mode/enable switches must never strand a voice. The invariant:
// once every trigger is released (note-offs sent AND every generator disabled), the
// dry output must fall completely silent. A hung voice keeps sounding forever.
//
// Black-box detector: run on the Init preset (dry — no FX tail), do the transition,
// release everything, pump well past the release time, and require the tail RMS ~ 0.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "PluginProcessor.h"
#include <cmath>

namespace
{
    void set01 (VASynthProcessor& p, const char* id, float v)
    { if (auto* pr = p.apvts.getParameter (id)) pr->setValueNotifyingHost (v); }
    void setVal (VASynthProcessor& p, const char* id, float v)
    { if (auto* pr = p.apvts.getParameter (id)) pr->setValueNotifyingHost (pr->convertTo0to1 (v)); }

    struct Rig
    {
        VASynthProcessor p; juce::AudioBuffer<float> buf; juce::MidiBuffer midi;
        Rig() : buf (2, 128) { p.prepareToPlay (48000.0, 128); p.loadInitPreset(); setVal (p, ParamID::ampRelease, 0.02f); }
        void pump (int blocks) { for (int b = 0; b < blocks; ++b) { buf.clear(); p.processBlock (buf, midi); } }
        // Settle past any release decay, then the PEAK of the steady-state tail. A released
        // synth reads ~0; a hung voice sustains at a real level forever. (Averaging RMS over
        // the release window would smear the decay in — peak-after-settle is the clean test.)
        float tailPeak (int settle = 250, int measure = 120)
        {
            pump (settle);
            float pk = 0.0f;
            for (int b = 0; b < measure; ++b)
            {
                buf.clear(); p.processBlock (buf, midi);
                for (int i = 0; i < buf.getNumSamples(); ++i)
                    pk = std::max ({ pk, std::abs (buf.getSample (0, i)), std::abs (buf.getSample (1, i)) });
            }
            return pk;
        }
        // Turn off every generator so nothing SHOULD sound anymore.
        void silenceAllTriggers()
        {
            set01 (p, ParamID::arpOn, 0.0f);
            set01 (p, ParamID::seqOn, 0.0f);
            set01 (p, ParamID::loopRec, 0.0f);
            set01 (p, ParamID::loopPlay, 0.0f);
            set01 (p, ParamID::chordEnabled, 0.0f);
        }
    };

    constexpr float kSilent = 1.0e-4f;   // peak: released ~0, a hung voice sustains far above this
}

TEST_CASE ("no hang: MIDI looper play toggled off mid-loop releases its notes", "[plugin][stuck][looper]")
{
    Rig r;
    set01 (r.p, ParamID::loopMode, 0.0f);      // MIDI
    setVal (r.p, ParamID::tempo, 200.0f);
    set01 (r.p, ParamID::loopRec, 1.0f);
    set01 (r.p, ParamID::loopPlay, 1.0f);
    r.pump (2);
    r.p.routeNoteOn (60, 0.9f, 0); r.pump (3); r.p.routeNoteOff (60, 0);   // record a short note
    r.pump (300);                                                          // let it loop a few times
    // Now cut the loop mid-cycle and release the live key. Nothing is triggering.
    r.silenceAllTriggers();
    r.p.routeNoteOff (60, 0);
    REQUIRE (r.tailPeak() < kSilent);
}

TEST_CASE ("no hang: switching loop MIDI->AUDIO mid-loop releases MIDI-lane notes", "[plugin][stuck][looper]")
{
    Rig r;
    set01 (r.p, ParamID::loopMode, 0.0f);      // start in MIDI
    setVal (r.p, ParamID::tempo, 200.0f);
    set01 (r.p, ParamID::loopRec, 1.0f);
    set01 (r.p, ParamID::loopPlay, 1.0f);
    r.pump (2);
    r.p.routeNoteOn (55, 0.9f, 0); r.pump (3); r.p.routeNoteOff (55, 0);
    r.pump (300);
    set01 (r.p, ParamID::loopMode, 1.0f);      // flip to AUDIO — MIDI lane stops dispatching
    r.pump (200);
    r.silenceAllTriggers();
    REQUIRE (r.tailPeak() < kSilent);
}

TEST_CASE ("no hang: sequencer disabled mid-gate releases its notes", "[plugin][stuck][seq]")
{
    Rig r;
    r.p.setSeqNote (0, 60);
    for (int s = 0; s < VASynthProcessor::kSeqSteps; ++s) r.p.setSeqCell (0, s, 1);   // row 0 every step
    set01 (r.p, ParamID::seqTarget, 0.0f);     // P1
    setVal (r.p, ParamID::seqGate, 0.9f);      // long gate -> a note is usually mid-flight
    setVal (r.p, ParamID::tempo, 160.0f);
    set01 (r.p, ParamID::seqOn, 1.0f);
    r.pump (200);                              // sequencer is running, a note is held
    set01 (r.p, ParamID::seqOn, 0.0f);         // disable mid-gate
    r.silenceAllTriggers();
    REQUIRE (r.tailPeak() < kSilent);
}

TEST_CASE ("no hang: arp disabled while notes held falls silent", "[plugin][stuck][arp]")
{
    Rig r;
    setVal (r.p, ParamID::tempo, 220.0f);
    set01 (r.p, ParamID::arpOn, 1.0f);
    r.pump (2);
    r.p.routeNoteOn (60, 0.9f, 0); r.p.routeNoteOn (64, 0.9f, 0); r.p.routeNoteOn (67, 0.9f, 0);
    r.pump (200);
    r.p.routeNoteOff (60, 0); r.p.routeNoteOff (64, 0); r.p.routeNoteOff (67, 0);
    r.silenceAllTriggers();                    // arp off
    REQUIRE (r.tailPeak() < kSilent);
}

TEST_CASE ("no hang: arp + seq both running, both disabled, releases everything", "[plugin][stuck][arp][seq]")
{
    Rig r;
    setVal (r.p, ParamID::tempo, 200.0f);
    set01 (r.p, ParamID::arpOn, 1.0f);
    r.p.setSeqNote (1, 62);
    for (int s = 0; s < VASynthProcessor::kSeqSteps; s += 2) r.p.setSeqCell (1, s, 1);
    set01 (r.p, ParamID::seqTarget, 0.0f);
    setVal (r.p, ParamID::seqGate, 0.8f);
    set01 (r.p, ParamID::seqOn, 1.0f);
    r.pump (2);
    r.p.routeNoteOn (48, 0.9f, 0); r.p.routeNoteOn (52, 0.9f, 0);
    r.pump (200);
    r.p.routeNoteOff (48, 0); r.p.routeNoteOff (52, 0);
    r.silenceAllTriggers();
    REQUIRE (r.tailPeak() < kSilent);
}

TEST_CASE ("no hang: chord held then chord engine disabled releases the chord tones", "[plugin][stuck][chord]")
{
    Rig r;
    set01 (r.p, ParamID::chordEnabled, 1.0f);
    r.pump (2);
    r.p.routeNoteOn (60, 0.9f, 0);             // one finger -> a triad
    r.pump (100);
    set01 (r.p, ParamID::chordEnabled, 0.0f);  // disable the engine while the triad is held
    r.p.routeNoteOff (60, 0);                  // release the finger
    r.silenceAllTriggers();
    REQUIRE (r.tailPeak() < kSilent);
}

TEST_CASE ("no hang: changing the patch under a held note then releasing falls silent", "[plugin][stuck][preset]")
{
    Rig r;
    r.p.routeNoteOn (60, 0.9f, 0);             // hold a note
    r.pump (50);
    r.p.loadFactoryPreset ("Fat Saw Bass");    // swap the sound out from under it
    r.pump (50);
    r.p.routeNoteOff (60, 0);                  // release
    r.silenceAllTriggers();
    REQUIRE (r.tailPeak() < kSilent);
}
