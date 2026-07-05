#pragma once
#include <array>
#include <cstddef>

// ============================================================================
// Computer-keyboard -> MIDI note mapping + clean note-on/off edge detection for
// the standalone. JUCE-free and unit-tested; the editor supplies an "is this key
// down" query (juce::KeyPress::isKeyCurrentlyDown) and an emit(note,on) sink.
//
// Per-key state tracking means OS auto-repeat is ignored entirely: a key that
// stays down produces exactly one note-on; releasing it produces exactly one
// note-off — no matter how many repeat callbacks arrive in between.
//
// Layout (US keymap assumed — JUCE exposes character codes, not portable
// physical scan codes; documented in README):
//
//     q w e r t y u i o p [ ]   -> C4..B4  (MIDI 60..71)   [octave shift 0]
//     1 2 3 4 5 6 7 8 9 0 - =   -> C5..B5  (MIDI 72..83)
//
//   z / x  : octave shift down / up (edge-triggered). A shift affects notes
//            pressed afterwards; already-held notes keep their pitch, and their
//            note-off carries the note that was actually sounded.
//
// Lowercase character codes match JUCE's own MidiKeyboardComponent keymap and
// work with KeyPress::isKeyCurrentlyDown on Linux/X11.
// ============================================================================

class QwertyKeyboard
{
public:
    static constexpr int kFirstNote = 60;      // 'q' at octave shift 0 = C4
    static constexpr int kNumKeys   = 24;

    QwertyKeyboard() { heldNote.fill (-1); }

    // Note for a key code at octave shift 0, or -1 if the key is unmapped.
    static int keyToNoteBase (int keyCode)
    {
        for (int i = 0; i < kNumKeys; ++i)
            if (kKeys[(std::size_t) i] == keyCode) return kFirstNote + i;
        return -1;
    }

    // Diff the current key state against what we last saw; call emit(note, isOn)
    // only on transitions. z/x shift the octave (edge-triggered too, so a held
    // z/x doesn't run the octave away under auto-repeat).
    template <class IsDown, class Emit>
    void update (IsDown isDown, Emit emit)
    {
        const bool zNow = isDown ('z'), xNow = isDown ('x');
        if (zNow && ! zHeld) setOctaveShift (octaveShift - 1);
        if (xNow && ! xHeld) setOctaveShift (octaveShift + 1);
        zHeld = zNow; xHeld = xNow;

        for (int i = 0; i < kNumKeys; ++i)
        {
            const bool down    = isDown (kKeys[(std::size_t) i]);
            const bool wasDown = heldNote[(std::size_t) i] >= 0;

            if (down && ! wasDown)
            {
                const int note = kFirstNote + i + 12 * octaveShift;   // in-range by clamp
                heldNote[(std::size_t) i] = note;
                emit (note, true);
            }
            else if (! down && wasDown)
            {
                emit (heldNote[(std::size_t) i], false);
                heldNote[(std::size_t) i] = -1;
            }
        }
    }

    // Release every currently-held note (focus loss / editor close). Idempotent.
    template <class Emit>
    void releaseAll (Emit emit)
    {
        zHeld = xHeld = false;
        for (int i = 0; i < kNumKeys; ++i)
            if (heldNote[(std::size_t) i] >= 0)
            {
                emit (heldNote[(std::size_t) i], false);
                heldNote[(std::size_t) i] = -1;
            }
    }

    int  getOctaveShift() const { return octaveShift; }
    bool anyHeld() const { for (int n : heldNote) if (n >= 0) return true; return false; }

private:
    // Clamp so all 24 keys stay in [0,127]: 60+12s >= 0 and 83+12s <= 127.
    static constexpr int kMinShift = -5;       // lowest key -> MIDI 0
    static constexpr int kMaxShift =  3;       // highest key -> MIDI 119
    void setOctaveShift (int s)
    {
        octaveShift = s < kMinShift ? kMinShift : (s > kMaxShift ? kMaxShift : s);
    }

    static constexpr std::array<int, kNumKeys> kKeys {{
        'q','w','e','r','t','y','u','i','o','p','[',']',
        '1','2','3','4','5','6','7','8','9','0','-','='
    }};

    std::array<int, kNumKeys> heldNote {};     // MIDI note sounding per key, or -1
    int  octaveShift = 0;
    bool zHeld = false, xHeld = false;
};
