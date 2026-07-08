// ============================================================================
// QWERTY computer-keyboard mapping + edge detection (JUCE-free logic).
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "QwertyKeyboard.h"
#include "PluginProcessor.h"
#include <set>
#include <vector>
#include <string>
#include <cmath>

namespace
{
    struct Ev { int note; bool on; };

    // Drive QwertyKeyboard from a set of "currently down" key codes.
    void step (QwertyKeyboard& kb, const std::string& down, std::vector<Ev>& out)
    {
        kb.update ([&](int kc){ return down.find ((char) kc) != std::string::npos; },
                   [&](int note, bool on){ out.push_back ({ note, on }); });
    }
}

TEST_CASE ("mapping table: all 24 keys map, both octaves, no duplicates", "[qwerty][map]")
{
    const std::string keys = "qwertyuiop[]1234567890-=";
    REQUIRE (keys.size() == (std::size_t) QwertyKeyboard::kNumKeys);

    std::set<int> notes;
    for (std::size_t i = 0; i < keys.size(); ++i)
    {
        const int n = QwertyKeyboard::keyToNoteBase (keys[i]);
        INFO ("key '" << keys[i] << "' -> " << n);
        REQUIRE (n == 60 + (int) i);          // q=60 (C4) .. '='=83 (B5), contiguous
        notes.insert (n);
    }
    REQUIRE (notes.size() == 24);             // no duplicates
    REQUIRE (*notes.begin() == 60);           // C4
    REQUIRE (*notes.rbegin() == 83);          // B5

    // Unmapped keys return -1.
    REQUIRE (QwertyKeyboard::keyToNoteBase ('z') == -1);
    REQUIRE (QwertyKeyboard::keyToNoteBase (' ') == -1);
}

TEST_CASE ("reserved bottom row (Phase 7 chord modifiers) is not consumed", "[qwerty][map][reserved]")
{
    // c v b n m , . /  are RESERVED for the Phase 7 chord modifiers. Nothing here
    // (note mapping OR the z/x octave controls) may claim them — a regression that
    // stole one of these would break the chord engine before it ships.
    const std::string reserved = "cvbnm,./";
    for (char k : reserved)
    {
        INFO ("reserved key '" << k << "' must be unmapped");
        REQUIRE (QwertyKeyboard::keyToNoteBase (k) == -1);      // not a note
    }

    // And pressing the whole reserved row emits NO note events and does NOT move
    // the octave (only z/x do that).
    QwertyKeyboard kb;
    std::vector<Ev> ev;
    step (kb, reserved, ev);
    step (kb, reserved, ev);
    REQUIRE (ev.empty());
    REQUIRE (kb.getOctaveShift() == 0);

    // Sanity: the note keys and the two octave keys (z/x) are the ONLY consumed
    // keys — the reserved row shares no code with them.
    const std::string consumed = "qwertyuiop[]1234567890-=zx";
    for (char k : reserved)
        REQUIRE (consumed.find (k) == std::string::npos);
}

TEST_CASE ("exactly one note-on/off per press-release, even under repeat storms", "[qwerty][edges]")
{
    QwertyKeyboard kb;
    std::vector<Ev> ev;

    step (kb, "q", ev);                       // press q
    for (int i = 0; i < 50; ++i) step (kb, "q", ev);   // 50 auto-repeat callbacks
    REQUIRE (ev.size() == 1);
    REQUIRE (ev[0].note == 60);
    REQUIRE (ev[0].on);

    step (kb, "", ev);                        // release q
    for (int i = 0; i < 50; ++i) step (kb, "", ev);    // more repeat noise
    REQUIRE (ev.size() == 2);
    REQUIRE (ev[1].note == 60);
    REQUIRE (! ev[1].on);
}

TEST_CASE ("chords: independent per-key on/off", "[qwerty][chord]")
{
    QwertyKeyboard kb;
    std::vector<Ev> ev;
    step (kb, "qet", ev);                     // chromatic: q=60, e=62, t=64 together
    REQUIRE (ev.size() == 3);
    step (kb, "qt", ev);                      // release the 'e' key (62) only
    REQUIRE (ev.size() == 4);
    REQUIRE (ev.back().note == 62);
    REQUIRE (! ev.back().on);
}

TEST_CASE ("z/x shift octave; held notes keep pitch, new notes shift", "[qwerty][octave]")
{
    QwertyKeyboard kb;
    std::vector<Ev> ev;

    step (kb, "q", ev);                        // C4 = 60
    REQUIRE (ev.back().note == 60);

    step (kb, "qx", ev);                       // x edge -> octave +1 (held q unchanged)
    REQUIRE (kb.getOctaveShift() == 1);
    step (kb, "qx", ev);                       // repeat x: no further shift
    REQUIRE (kb.getOctaveShift() == 1);
    const std::size_t afterShift = ev.size();

    step (kb, "wx", ev);                       // release q -> off 60; press w (base 61) -> 61+12 = 73
    bool sawOff60 = false, sawOn73 = false;
    for (std::size_t i = afterShift; i < ev.size(); ++i)
    {
        if (ev[i].note == 60 && ! ev[i].on) sawOff60 = true;
        if (ev[i].note == 73 &&   ev[i].on) sawOn73 = true;
    }
    REQUIRE (sawOff60);                         // released note carried its original pitch
    REQUIRE (sawOn73);                          // new note took the shifted octave

    // z shifts back down.
    step (kb, "wz", ev);                        // z edge -> octave 0
    REQUIRE (kb.getOctaveShift() == 0);
}

TEST_CASE ("octave shift clamps to valid MIDI range", "[qwerty][octave]")
{
    QwertyKeyboard kb;
    std::vector<Ev> ev;
    for (int i = 0; i < 10; ++i) { step (kb, "x", ev); step (kb, "", ev); }   // spam up
    REQUIRE (kb.getOctaveShift() == 3);          // clamped (highest key -> MIDI 119)
    for (int i = 0; i < 20; ++i) { step (kb, "z", ev); step (kb, "", ev); }   // spam down
    REQUIRE (kb.getOctaveShift() == -5);         // clamped (lowest key -> MIDI 0)
}

TEST_CASE ("qwerty notes flow through processBlock to audio (merge path)", "[qwerty][integration]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 256);

    auto rmsOfBlocks = [&](int nBlocks)
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

    // Feed a note the way the editor does: through the QWERTY surface zones.
    p.routeSurfaceMessage ("QWERTY", juce::MidiMessage::noteOn (1, 60, 0.8f));
    REQUIRE (rmsOfBlocks (20) > 0.01);          // it sounds

    p.routeSurfaceMessage ("QWERTY", juce::MidiMessage::noteOff (1, 60));
    rmsOfBlocks (60);                            // let the release finish
    REQUIRE (rmsOfBlocks (20) < 1.0e-3);         // then silent
}

TEST_CASE ("releaseAll (focus loss / close) frees every held note exactly once", "[qwerty][focus]")
{
    QwertyKeyboard kb;
    std::vector<Ev> ev;
    step (kb, "qwe", ev);                        // three held
    ev.clear();

    kb.releaseAll ([&](int note, bool on){ ev.push_back ({ note, on }); });
    REQUIRE (ev.size() == 3);                     // exactly the three held notes
    for (auto& e : ev) REQUIRE (! e.on);
    REQUIRE (! kb.anyHeld());

    kb.releaseAll ([&](int note, bool on){ ev.push_back ({ note, on }); });
    REQUIRE (ev.size() == 3);                     // idempotent: nothing more
}
