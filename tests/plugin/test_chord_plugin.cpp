// ============================================================================
// 7B chord engine — processor integration. The pure grammar is exhaustively
// covered by dsp/test_chord.cpp; here we verify the wiring: expansion in the
// dispatch path, chord-forces-poly, momentary modifiers via the QWERTY mask,
// learning modifiers from a CC and from a consumed note, persistence, sustain,
// and RT-safety.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "PluginProcessor.h"
#include "alloc_hook.h"
#include <cmath>

namespace
{
    void setP (VASynthProcessor& p, const char* id, float v)
    {
        p.apvts.getParameter (id)->setValueNotifyingHost (v);
    }

    void pump (VASynthProcessor& p, juce::MidiBuffer midi, int blocks = 1)
    {
        juce::AudioBuffer<float> buf (2, 256);
        for (int b = 0; b < blocks; ++b) { buf.clear(); juce::MidiBuffer m = (b == 0 ? midi : juce::MidiBuffer{}); p.processBlock (buf, m); }
    }

    // RMS while a QWERTY note is held, then release + settle.
    double playRms (VASynthProcessor& p, int note, int blocks = 24)
    {
        p.qwertyKeyboardState.noteOn (1, note, 0.8f);
        juce::AudioBuffer<float> buf (2, 256);
        double acc = 0.0; long n = 0;
        for (int b = 0; b < blocks; ++b)
        {
            buf.clear(); juce::MidiBuffer m; p.processBlock (buf, m);
            const float* L = buf.getReadPointer (0);
            for (int i = 0; i < 256; ++i) { acc += double (L[i]) * L[i]; ++n; }
        }
        p.qwertyKeyboardState.noteOff (1, note, 0.0f);
        pump (p, {}, 90);                                  // settle to silence
        return std::sqrt (acc / (double) n);
    }
}

TEST_CASE ("chord ON expands one played note into more voices", "[plugin][7b][chord]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p; p.prepareToPlay (48000.0, 256);
    setP (p, ParamID::chordRoot, 0.0f); setP (p, ParamID::chordScale, 0.0f);   // C major

    setP (p, ParamID::chordEnabled, 0.0f);
    const double off = playRms (p, 60);
    setP (p, ParamID::chordEnabled, 1.0f);
    const double on  = playRms (p, 60);
    INFO ("off=" << off << " on=" << on);
    REQUIRE (on > off * 1.4);                              // a triad is clearly fuller than one note
}

TEST_CASE ("chord mode forces poly (a chord sounds even in Mono)", "[plugin][7b][chord][poly]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p; p.prepareToPlay (48000.0, 256);
    setP (p, ParamID::chordRoot, 0.0f); setP (p, ParamID::chordScale, 0.0f);
    setP (p, ParamID::polyMode, 1.0f);                     // Mono (normalized 1.0 -> index... choice)

    setP (p, ParamID::chordEnabled, 0.0f);
    const double mono1 = playRms (p, 60);                  // mono, chord off -> 1 note
    setP (p, ParamID::chordEnabled, 1.0f);
    const double chord = playRms (p, 60);                  // chord forces poly -> triad
    INFO ("mono1=" << mono1 << " chord=" << chord);
    REQUIRE (chord > mono1 * 1.4);
}

TEST_CASE ("QWERTY modifier mask engages a modifier", "[plugin][7b][chord][modifier]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p; p.prepareToPlay (48000.0, 256);
    setP (p, ParamID::chordEnabled, 1.0f);

    p.setQwertyChordModifiers (1u << ChordEngine::ModMin);
    pump (p, {});
    REQUIRE (p.isModifierActive (ChordEngine::ModMin));
    p.setQwertyChordModifiers (0);
    pump (p, {});
    REQUIRE_FALSE (p.isModifierActive (ChordEngine::ModMin));
}

TEST_CASE ("learn a modifier from a CC (footswitch)", "[plugin][7b][chord][learn][cc]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p; p.prepareToPlay (48000.0, 256);
    setP (p, ParamID::chordEnabled, 1.0f);
    auto& ml = p.getModifierLearn();

    ml.armLearn (ChordEngine::ModMaj);
    juce::MidiBuffer down; down.addEvent (juce::MidiMessage::controllerEvent (1, 80, 127), 0);
    pump (p, down);
    REQUIRE (ml.getCCForModifier (ChordEngine::ModMaj) == 80);
    REQUIRE (p.isModifierActive (ChordEngine::ModMaj));     // pressed -> held

    juce::MidiBuffer up; up.addEvent (juce::MidiMessage::controllerEvent (1, 80, 0), 0);
    pump (p, up);
    REQUIRE_FALSE (p.isModifierActive (ChordEngine::ModMaj));
}

TEST_CASE ("learn a modifier from a note (pad) — the note is consumed", "[plugin][7b][chord][learn][note]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p; p.prepareToPlay (48000.0, 256);
    setP (p, ParamID::chordEnabled, 1.0f);
    auto& ml = p.getModifierLearn();

    ml.armLearn (ChordEngine::ModDim);
    juce::MidiBuffer padOn; padOn.addEvent (juce::MidiMessage::noteOn (1, 24, 1.0f), 0);
    pump (p, padOn);
    REQUIRE (ml.getNoteForModifier (ChordEngine::ModDim) == 24);
    REQUIRE (p.isModifierActive (ChordEngine::ModDim));      // held

    // Consumed, not played: with only the pad note in, the output stays silent.
    double acc = 0.0; long n = 0; juce::AudioBuffer<float> buf (2, 256);
    for (int b = 0; b < 8; ++b) { buf.clear(); juce::MidiBuffer m; p.processBlock (buf, m);
        const float* L = buf.getReadPointer (0); for (int i = 0; i < 256; ++i) { acc += double (L[i]) * L[i]; ++n; } }
    REQUIRE (std::sqrt (acc / (double) n) < 1.0e-4);        // the pad made no sound

    juce::MidiBuffer padOff; padOff.addEvent (juce::MidiMessage::noteOff (1, 24), 0);
    pump (p, padOff);
    REQUIRE_FALSE (p.isModifierActive (ChordEngine::ModDim));
}

TEST_CASE ("modifier-learn mappings persist across a state round-trip", "[plugin][7b][chord][learn][state]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor src; src.prepareToPlay (48000.0, 256);
    setP (src, ParamID::chordEnabled, 1.0f);
    src.getModifierLearn().armLearn (ChordEngine::ModSus4);
    juce::MidiBuffer learn; learn.addEvent (juce::MidiMessage::controllerEvent (1, 90, 127), 0);
    pump (src, learn);
    juce::MemoryBlock blob; src.getStateInformation (blob);

    VASynthProcessor dst; dst.prepareToPlay (48000.0, 256);
    dst.setStateInformation (blob.getData(), (int) blob.getSize());
    setP (dst, ParamID::chordEnabled, 1.0f);
    REQUIRE (dst.getModifierLearn().getCCForModifier (ChordEngine::ModSus4) == 90);
    juce::MidiBuffer press; press.addEvent (juce::MidiMessage::controllerEvent (1, 90, 127), 0);
    pump (dst, press);
    REQUIRE (dst.isModifierActive (ChordEngine::ModSus4));   // the restored mapping works
}

TEST_CASE ("sustain pedal holds chord tones until release", "[plugin][7b][chord][sustain]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p; p.prepareToPlay (48000.0, 256);
    setP (p, ParamID::chordEnabled, 1.0f);
    setP (p, ParamID::ampSustain, 1.0f);

    auto rms = [&] { juce::AudioBuffer<float> buf (2, 256); double acc = 0; long n = 0;
        for (int b = 0; b < 8; ++b) { buf.clear(); juce::MidiBuffer m; p.processBlock (buf, m);
            const float* L = buf.getReadPointer (0); for (int i = 0; i < 256; ++i) { acc += double (L[i]) * L[i]; ++n; } }
        return std::sqrt (acc / (double) n); };

    juce::MidiBuffer ped; ped.addEvent (juce::MidiMessage::controllerEvent (1, 64, 127), 0); pump (p, ped);
    juce::MidiBuffer on; on.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0); pump (p, on);
    REQUIRE (rms() > 0.01);                                 // chord sounding
    juce::MidiBuffer off; off.addEvent (juce::MidiMessage::noteOff (1, 60), 0); pump (p, off);
    REQUIRE (rms() > 0.01);                                 // pedal holds the chord tones
    juce::MidiBuffer pedUp; pedUp.addEvent (juce::MidiMessage::controllerEvent (1, 64, 0), 0); pump (p, pedUp);
    pump (p, {}, 90);
    REQUIRE (rms() < 0.005);                                // released on pedal-up
}

TEST_CASE ("chord expansion path does not allocate on the audio thread", "[plugin][7b][chord][rt]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p; p.prepareToPlay (48000.0, 512);
    setP (p, ParamID::chordEnabled, 1.0f);
    p.setQwertyChordModifiers (1u << ChordEngine::ModMin);

    juce::AudioBuffer<float> buf (2, 512);
    for (int i = 0; i < 20; ++i) { buf.clear(); juce::MidiBuffer m; p.processBlock (buf, m); }   // warm up

    {
        alloc_hook::AllocGuard g;
        for (int b = 0; b < 200; ++b)
        {
            buf.clear();
            juce::MidiBuffer m;
            if (b % 8 == 0)  m.addEvent (juce::MidiMessage::noteOn  (1, 60 + (b % 5), 0.8f), 0);
            if (b % 8 == 4)  m.addEvent (juce::MidiMessage::noteOff (1, 60 + (b % 5)), 0);
            p.processBlock (buf, m);
        }
        REQUIRE (g.count() == 0);
    }
}
