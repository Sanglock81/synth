// ============================================================================
// Click/pop torture for the note-generating features (arp / looper / chord-morph).
// STANDING RULE (R3): every change that generates notes or touches the audio path
// ships WITH click-torture coverage of its specific behavior, and this suite is part
// of every gate. Noise cleanliness is a permanent regression criterion.
//
// Each scenario drives the FULL processor and scans the stereo output for
// sample-to-sample discontinuities (clicks/pops), out-of-range peaks, and non-finite
// samples. A discontinuity threshold well under a note attack's natural slope catches
// a retrigger/skip pop without flagging legitimate onsets.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "PluginProcessor.h"
#include <cmath>
#include <functional>

namespace
{
    struct Scan { float maxJump = 0, peak = 0; bool finite = true; };

    void set01 (VASynthProcessor& p, const char* id, float v01)
    { if (auto* pr = p.apvts.getParameter (id)) pr->setValueNotifyingHost (v01); }
    void setVal (VASynthProcessor& p, const char* id, float v)
    { if (auto* pr = p.apvts.getParameter (id)) pr->setValueNotifyingHost (pr->convertTo0to1 (v)); }

    // Pump `blocks` blocks; `fill(midi, b)` injects host MIDI for block b. Scans output.
    struct Pumper
    {
        VASynthProcessor& p; juce::AudioBuffer<float> buf; float prevL = 0, prevR = 0; Scan s;
        explicit Pumper (VASynthProcessor& proc, int bs) : p (proc), buf (2, bs) {}
        void pump (int blocks, std::function<void(juce::MidiBuffer&, int)> fill = {})
        {
            for (int b = 0; b < blocks; ++b)
            {
                juce::MidiBuffer m; if (fill) fill (m, b);
                buf.clear(); p.processBlock (buf, m);
                const float* L = buf.getReadPointer (0); const float* R = buf.getReadPointer (1);
                for (int i = 0; i < buf.getNumSamples(); ++i)
                {
                    s.finite = s.finite && std::isfinite (L[i]) && std::isfinite (R[i]);
                    s.peak = std::max ({ s.peak, std::abs (L[i]), std::abs (R[i]) });
                    s.maxJump = std::max ({ s.maxJump, std::abs (L[i] - prevL), std::abs (R[i] - prevR) });
                    prevL = L[i]; prevR = R[i];
                }
            }
        }
    };

    // Discontinuity ceiling. Reference: a clean 3-note SAW chord (the brightest default
    // patch) measures ~0.196 sample-to-sample; every arp/looper stress scenario here
    // measures ~0.10-0.12. A genuine retrigger/skip pop jumps toward the peak-to-peak
    // (>0.4), so 0.35 catches pops with margin above the honest waveform slope.
    constexpr float kClick = 0.35f;

    // A minimal host playhead for the J1 synced-LFO torture: an advancing ppq that can be
    // snapped (loop brace / play) or re-rated (tempo change) mid-stream.
    struct MockHost : juce::AudioPlayHead
    {
        double bpm = 120.0, ppq = 0.0; bool playing = true;
        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo pi; pi.setBpm (bpm); pi.setPpqPosition (ppq); pi.setIsPlaying (playing);
            return pi;
        }
    };

    // Set up a held-note patch whose LFO 1 is a deep, tempo-synced modulation of the FILTER
    // cutoff — the worst case for a phase-discontinuity click (a cutoff step is audible).
    void armSyncedCutoffLfo (VASynthProcessor& p)
    {
        p.loadInitPreset();
        setVal (p, ParamID::filterCutoff, 800.0f);     // mid cutoff so the LFO swings audibly
        set01 (p, ParamID::lfoDest, 2.0f / 3.0f);      // Dest = Cutoff (index 2 of 4)
        setVal (p, ParamID::lfoDepth, 0.9f);           // deep -> a phase jump moves cutoff a lot
        set01 (p, ParamID::lfoDiv, 4.0f / 13.0f);      // 1/4 (index 4 of 14)
    }
}

TEST_CASE ("BASELINE: held saw chord, no arp (waveform reference)", "[plugin][click][baseline]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    p.loadInitPreset();
    Pumper pm (p, 128);
    pm.pump (10);
    p.routeNoteOn (60, 0.9f, 0); p.routeNoteOn (63, 0.9f, 0); p.routeNoteOn (67, 0.9f, 0);
    pm.pump (500);
    INFO ("BASELINE peak=" << pm.s.peak << " maxJump=" << pm.s.maxJump);
    REQUIRE (pm.s.maxJump < kClick);
}

TEST_CASE ("torture: fast short-gate arp over a chord stays click-free (FX off)", "[plugin][click][arp]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    p.loadInitPreset();
    setVal (p, ParamID::tempo, 280.0f);        // fast clock -> rapid retriggers
    set01 (p, ParamID::arpGate, 0.12f);        // short gate -> off/on churn every step
    set01 (p, ParamID::arpOn, 1.0f);

    Pumper pm (p, 128);
    pm.pump (10);
    p.routeNoteOn (60, 0.9f, 0); p.routeNoteOn (63, 0.9f, 0); p.routeNoteOn (67, 0.9f, 0);   // Cm
    pm.pump (2000);                            // ~5.3 s of arping
    INFO ("peak=" << pm.s.peak << " maxJump=" << pm.s.maxJump);
    REQUIRE (pm.s.finite);
    REQUIRE (pm.s.peak <= 1.0f);
    REQUIRE (pm.s.maxJump < kClick);
}

TEST_CASE ("torture: arp gate pattern with rests, sparse retriggers stay click-free", "[plugin][click][arp][gate]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    p.loadInitPreset();
    setVal (p, ParamID::ampRelease, 0.002f);   // fast release -> each rest cuts a note hard
    setVal (p, ParamID::tempo, 280.0f);
    set01 (p, ParamID::arpGate, 0.14f);
    // A syncopated on/off gate: rests interleaved with hits (the note-off on a rest edge
    // is the click risk the standing rule guards).
    for (int i = 0; i < VASynthProcessor::kArpSteps; ++i) p.setArpStep (i, (i % 3 == 0) ? 1.0f : 0.0f);
    set01 (p, ParamID::arpOn, 1.0f);

    Pumper pm (p, 128);
    pm.pump (10);
    p.routeNoteOn (60, 0.9f, 0); p.routeNoteOn (64, 0.9f, 0); p.routeNoteOn (67, 0.9f, 0);
    pm.pump (2500);
    INFO ("peak=" << pm.s.peak << " maxJump=" << pm.s.maxJump);
    REQUIRE (pm.s.finite);
    REQUIRE (pm.s.peak <= 1.0f);
    REQUIRE (pm.s.maxJump < kClick);
}

TEST_CASE ("torture: single-note arp, instant release, retrigger every step", "[plugin][click][arp][retrig]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    p.loadInitPreset();
    setVal (p, ParamID::ampRelease, 0.001f);   // near-instant release -> hard cut each step
    setVal (p, ParamID::ampAttack,  0.001f);   // near-instant attack -> hard onset
    setVal (p, ParamID::tempo, 280.0f);
    set01 (p, ParamID::arpGate, 0.1f);
    set01 (p, ParamID::arpOn, 1.0f);

    Pumper pm (p, 128);
    pm.pump (10);
    p.routeNoteOn (60, 0.9f, 0);               // ONE note -> same pitch retriggers every step
    pm.pump (2000);
    INFO ("peak=" << pm.s.peak << " maxJump=" << pm.s.maxJump);
    REQUIRE (pm.s.finite);
    REQUIRE (pm.s.peak <= 1.0f);
    REQUIRE (pm.s.maxJump < kClick);
}

TEST_CASE ("torture: arp with reverb - rhythmic gaps don't pop the FX", "[plugin][click][arp][fx]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    p.loadInitPreset();
    set01 (p, ParamID::fxReverbOn, 1.0f); setVal (p, ParamID::reverbMix, 0.4f);
    set01 (p, ParamID::fxDelayOn, 1.0f);  setVal (p, ParamID::delayMix, 0.3f);
    setVal (p, ParamID::tempo, 150.0f);
    set01 (p, ParamID::arpGate, 0.2f);         // big silent gaps between notes
    set01 (p, ParamID::arpOn, 1.0f);

    Pumper pm (p, 128);
    pm.pump (10);
    p.routeNoteOn (48, 0.9f, 0); p.routeNoteOn (55, 0.9f, 0);
    pm.pump (3000);
    INFO ("peak=" << pm.s.peak << " maxJump=" << pm.s.maxJump);
    REQUIRE (pm.s.finite);
    REQUIRE (pm.s.peak <= 1.0f);
    REQUIRE (pm.s.maxJump < kClick);
}

TEST_CASE ("torture: looper overdub onto identical pitches while live-playing", "[plugin][click][looper]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    p.loadInitPreset();
    setVal (p, ParamID::tempo, 240.0f);
    set01 (p, ParamID::loopRec, 1.0f);
    set01 (p, ParamID::loopPlay, 1.0f);

    Pumper pm (p, 128);
    pm.pump (5);
    // Record a phrase on note 60, let it loop, then live-play 60 again over the playback
    // (identical pitch layered by loop + live).
    for (int cycle = 0; cycle < 8; ++cycle)
    {
        p.routeNoteOn (60, 0.9f, 0); pm.pump (6); p.routeNoteOff (60, 0); pm.pump (6);
        p.routeNoteOn (60, 0.8f, 0); pm.pump (4); p.routeNoteOff (60, 0); pm.pump (30);
    }
    INFO ("peak=" << pm.s.peak << " maxJump=" << pm.s.maxJump);
    REQUIRE (pm.s.finite);
    REQUIRE (pm.s.peak <= 1.0f);
    REQUIRE (pm.s.maxJump < kClick);
}

TEST_CASE ("torture: dense 8-row sequencer pattern at high tempo (Group 2)", "[plugin][click][seq]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    p.loadInitPreset();
    // A dense pattern: every row on every step (some accents), sounding synth notes.
    for (int r = 0; r < VASynthProcessor::kSeqRows; ++r)
    {
        p.setSeqNote (r, 48 + r * 2);
        for (int s = 0; s < VASynthProcessor::kSeqSteps; ++s) p.setSeqCell (r, s, (s % 4 == 0) ? 2 : 1);
    }
    set01 (p, ParamID::seqTarget, 0.0f);           // target P1
    setVal (p, ParamID::tempo, 260.0f);            // fast
    set01 (p, ParamID::seqGate, 0.15f);            // short gates -> lots of on/off churn
    set01 (p, ParamID::seqOn, 1.0f);

    Pumper pm (p, 128);
    pm.pump (3000);
    INFO ("peak=" << pm.s.peak << " maxJump=" << pm.s.maxJump);
    REQUIRE (pm.s.finite);
    REQUIRE (pm.s.peak <= 1.0f);
    REQUIRE (pm.s.maxJump < kClick);
}

TEST_CASE ("torture: AUDIO looper capture + wrap + playback stays click-free (Group 3)", "[plugin][click][looper][audio]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    p.loadInitPreset();
    setVal (p, ParamID::ampRelease, 0.05f);    // note decays to silence well inside the loop
    setVal (p, ParamID::tempo, 160.0f);        // ~1.5 s loop, longer than the note
    set01 (p, ParamID::loopMode, 1.0f);        // AUDIO playback
    set01 (p, ParamID::loopPlay, 1.0f);
    set01 (p, ParamID::loopRec, 1.0f);         // arms; engages at the boundary (pos 0)

    Pumper pm (p, 128);
    pm.pump (4);
    // A note just after the boundary: the loop starts in silence (pre-note) and ends in
    // silence (post-decay), so the wrap seam is silence->silence — the mechanism must not
    // add a pop across many wraps.
    p.routeNoteOn (60, 0.9f, 0); pm.pump (8); p.routeNoteOff (60, 0);
    pm.pump (560);                             // finish recording one loop (note decays, wraps)
    set01 (p, ParamID::loopRec, 0.0f);         // stop recording; keep looping
    pm.pump (1800);                            // many wraps, playback only
    INFO ("peak=" << pm.s.peak << " maxJump=" << pm.s.maxJump);
    REQUIRE (pm.s.finite);
    REQUIRE (pm.s.peak <= 1.0f);
    REQUIRE (pm.s.maxJump < kClick);
}

TEST_CASE ("torture: chord modifier morph churn under held notes (1.4)", "[plugin][click][chord]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    p.loadInitPreset();
    set01 (p, ParamID::chordEnabled, 1.0f);

    Pumper pm (p, 128);
    pm.pump (10);
    p.routeNoteOn (60, 0.9f, 0); p.routeNoteOn (64, 0.9f, 0); p.routeNoteOn (67, 0.9f, 0);   // hold 3 triggers
    const std::uint32_t MIN = 1u << ChordEngine::ModMin,  MAJ = 1u << ChordEngine::ModMaj;
    const std::uint32_t D7  = 1u << ChordEngine::ModDom7, S7  = 1u << ChordEngine::Mod7th;
    const std::uint32_t masks[] { MIN, MAJ, MIN | S7, D7, MAJ | S7, 0u };
    for (int c = 0; c < 24; ++c) { p.setQwertyChordModifiers (masks[c % 6]); pm.pump (15); }   // morph the held chords

    INFO ("peak=" << pm.s.peak << " maxJump=" << pm.s.maxJump);
    REQUIRE (pm.s.finite);
    REQUIRE (pm.s.peak <= 1.0f);
    REQUIRE (pm.s.maxJump < kClick);
}

// ---- J1.2 synced-LFO transition click-safety (amendment #2) ---------------------------
// All three phase-discontinuity moments, each on a deep cutoff-routed synced LFO, must stay
// below the click ceiling. These prove (not assert) the modulation smoothing absorbs the step.

TEST_CASE ("torture: toggle LFO SYNC on then off mid-note (bar-engage, frozen rate)", "[plugin][click][lfo][sync]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    armSyncedCutoffLfo (p);
    setVal (p, ParamID::tempo, 120.0f);            // standalone free-run beat clock

    Pumper pm (p, 128);
    pm.pump (10);
    p.routeNoteOn (48, 0.9f, 0);                    // a held, sustained note
    pm.pump (200);                                  // free-running LFO settles
    set01 (p, ParamID::lfoSync, 1.0f);             // SYNC on -> engages at the NEXT bar, no instant jump
    pm.pump (800);                                  // cross several bars (engage happens here)
    set01 (p, ParamID::lfoSync, 0.0f);             // SYNC off -> freeze current rate as free Hz, no jump
    pm.pump (800);
    INFO ("peak=" << pm.s.peak << " maxJump=" << pm.s.maxJump);
    REQUIRE (pm.s.finite);
    REQUIRE (pm.s.peak <= 1.0f);
    REQUIRE (pm.s.maxJump < kClick);
}

TEST_CASE ("torture: host ppq snap (play + loop-brace) under a synced cutoff LFO", "[plugin][click][lfo][sync][host]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    armSyncedCutoffLfo (p);
    set01 (p, ParamID::lfoSync, 1.0f);
    MockHost host; host.bpm = 120.0; host.playing = true; p.setPlayHead (&host);
    const double beatsPerBlock = (host.bpm / 60.0) * (128.0 / 48000.0);

    juce::AudioBuffer<float> buf (2, 128); float prevL = 0, prevR = 0; Scan s;
    auto run = [&] (int blocks)
    {
        for (int b = 0; b < blocks; ++b)
        {
            juce::MidiBuffer m; buf.clear(); p.processBlock (buf, m); host.ppq += beatsPerBlock;
            const float* L = buf.getReadPointer (0); const float* R = buf.getReadPointer (1);
            for (int i = 0; i < 128; ++i)
            {
                s.finite = s.finite && std::isfinite (L[i]) && std::isfinite (R[i]);
                s.peak = std::max ({ s.peak, std::abs (L[i]), std::abs (R[i]) });
                s.maxJump = std::max ({ s.maxJump, std::abs (L[i] - prevL), std::abs (R[i] - prevR) });
                prevL = L[i]; prevR = R[i];
            }
        }
    };
    run (10);
    p.routeNoteOn (48, 0.9f, 0);
    run (300);
    host.ppq = 7.37;              // loop brace snaps to a mid-bar, non-integer ppq
    run (300);
    host.ppq = 0.0;               // stop/restart from the top (play from bar 1)
    run (300);
    INFO ("peak=" << s.peak << " maxJump=" << s.maxJump);
    REQUIRE (s.finite);
    REQUIRE (s.peak <= 1.0f);
    REQUIRE (s.maxJump < kClick);
    p.setPlayHead (nullptr);
}

TEST_CASE ("torture: live host tempo change re-rating a synced cutoff LFO", "[plugin][click][lfo][sync][tempo]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    armSyncedCutoffLfo (p);
    set01 (p, ParamID::lfoSync, 1.0f);
    MockHost host; host.bpm = 90.0; host.playing = true; p.setPlayHead (&host);

    juce::AudioBuffer<float> buf (2, 128); float prevL = 0, prevR = 0; Scan s;
    auto run = [&] (int blocks)
    {
        for (int b = 0; b < blocks; ++b)
        {
            juce::MidiBuffer m; buf.clear(); p.processBlock (buf, m);
            host.ppq += (host.bpm / 60.0) * (128.0 / 48000.0);
            const float* L = buf.getReadPointer (0); const float* R = buf.getReadPointer (1);
            for (int i = 0; i < 128; ++i)
            {
                s.finite = s.finite && std::isfinite (L[i]) && std::isfinite (R[i]);
                s.peak = std::max ({ s.peak, std::abs (L[i]), std::abs (R[i]) });
                s.maxJump = std::max ({ s.maxJump, std::abs (L[i] - prevL), std::abs (R[i] - prevR) });
                prevL = L[i]; prevR = R[i];
            }
        }
    };
    run (10);
    p.routeNoteOn (48, 0.9f, 0);
    run (300);
    host.bpm = 174.0;             // sudden tempo jump (near-double) re-rates the LFO live
    run (300);
    host.bpm = 60.0;             // and back down
    run (300);
    INFO ("peak=" << s.peak << " maxJump=" << s.maxJump);
    REQUIRE (s.finite);
    REQUIRE (s.peak <= 1.0f);
    REQUIRE (s.maxJump < kClick);
    p.setPlayHead (nullptr);
}

// ---- J3 scene-transition click-safety -------------------------------------------------
// Launching a scene mid-phrase swaps the loop clips; the flip must flush any notes the old
// clip left sounding so the incoming scene starts clean. Record a loop, let it play, then
// churn scene launches across boundaries and scan for pops.
TEST_CASE ("torture: scene launches under a playing loop stay click-free (J3)", "[plugin][click][scene]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    p.loadInitPreset();
    setVal (p, ParamID::tempo, 240.0f);              // 1 bar = 375 blocks
    setVal (p, ParamID::ampRelease, 0.02f);
    set01 (p, ParamID::sceneQuant, 0.0f);            // 1-bar launch quantum
    p.apvts.getParameter (ParamID::loopBars)->setValueNotifyingHost (
        p.apvts.getParameter (ParamID::loopBars)->convertTo0to1 (0.0f));   // lane 0 = 1 bar

    Pumper pm (p, 128);
    // Record a 1-bar loop of a held-ish note on lane 0, let the one-shot auto-play.
    set01 (p, ParamID::loopRec, 1.0f);
    for (int i = 0; i < 4000 && p.loopRecDisplayState (0) != 2; ++i) pm.pump (1);
    p.routeNoteOn (60, 0.9f, 0); pm.pump (8); p.routeNoteOff (60, 0);
    for (int i = 0; i < 4000 && p.loopRecDisplayState (0) == 2; ++i) pm.pump (1);   // finish the take -> plays

    // Now churn scene launches: 0 -> 1 (empty) -> 0 ... each crossing a bar boundary mid-loop.
    for (int cycle = 0; cycle < 8; ++cycle)
    {
        p.launchScene ((cycle % 2 == 0) ? 1 : 0);
        pm.pump (420);                               // > 1 bar, so the pending scene engages and plays
    }
    INFO ("peak=" << pm.s.peak << " maxJump=" << pm.s.maxJump);
    REQUIRE (pm.s.finite);
    REQUIRE (pm.s.peak <= 1.0f);
    REQUIRE (pm.s.maxJump < kClick);
}

TEST_CASE ("torture: HOLD chord-replacement churn", "[plugin][click][arp][hold]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    p.loadInitPreset();
    setVal (p, ParamID::tempo, 240.0f);
    set01 (p, ParamID::arpHold, 1.0f);
    set01 (p, ParamID::arpOn, 1.0f);

    Pumper pm (p, 128);
    pm.pump (10);
    const int chords[3][3] = { { 60, 63, 67 }, { 62, 65, 69 }, { 59, 62, 67 } };
    for (int cycle = 0; cycle < 10; ++cycle)
    {
        const auto& c = chords[cycle % 3];
        for (int n : c) p.routeNoteOn (n, 0.9f, 0);
        pm.pump (20);
        for (int n : c) p.routeNoteOff (n, 0);       // released, but HELD -> keeps arping
        pm.pump (40);                                 // new chord next cycle replaces it
    }
    INFO ("peak=" << pm.s.peak << " maxJump=" << pm.s.maxJump);
    REQUIRE (pm.s.finite);
    REQUIRE (pm.s.peak <= 1.0f);
    REQUIRE (pm.s.maxJump < kClick);
}
