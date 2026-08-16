// Synth — Copyright © 2026 John L Farmer. Licensed under AGPLv3; see LICENSE.
#pragma once
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include "MidiTracer.h"
#include <fstream>
#include <cstdlib>

// ============================================================================
// G1.2 — the message-thread half of the MIDI trace: drains mtrace's lock-free ring
// to a text file. Constructed by the processor; a no-op unless VASYNTH_MIDI_TRACE=1.
//
// The audio thread only ever pushes PODs (mtrace::emit); this Timer (message thread)
// pops them every ~200 ms and appends one line per event, plus a final flush at
// teardown. File path: $VASYNTH_MIDI_TRACE_FILE, else ~/vasynth-miditrace.log.
// ============================================================================

class MidiTraceWriter : private juce::Timer
{
public:
    MidiTraceWriter()
    {
        if (! mtrace::on()) return;                              // disabled: no file, no timer

        const char* p = std::getenv ("VASYNTH_MIDI_TRACE_FILE");
        path = (p != nullptr && p[0] != '\0')
                 ? juce::String (juce::CharPointer_UTF8 (p))
                 : juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                       .getChildFile ("vasynth-miditrace.log").getFullPathName();

        out.open (path.toRawUTF8(), std::ios::out | std::ios::trunc);
        if (out.is_open())
        {
            out << "# VASynth MIDI trace  columns: block frame kind a b c d\n"
                   "# kinds: BLK=block NLIV/NGEN=note-on(live/gen) NRTG=retrig NOFF=note-off "
                   "STEAL=voice-steal CC PANIC ANOFF=all-notes-off LREC=loop-record "
                   "LEMIT=loop-playback LWRAP=lane-wrap-arm\n";
            out.flush();
            startTimer (200);
        }
    }

    ~MidiTraceWriter() override
    {
        if (! out.is_open()) return;
        stopTimer();
        flush();
        out << "# end (dropped=" << (unsigned long long) mtrace::tracer().dropped() << ")\n";
        out.close();
    }

private:
    void timerCallback() override { flush(); }

    void flush()
    {
        if (! out.is_open()) return;
        mtrace::tracer().drain ([this] (const mtrace::Event& e)
        {
            out << e.block << ' ' << e.frame << ' ' << kindName (e.kind) << ' '
                << e.a << ' ' << e.b << ' ' << e.c << ' ' << e.d << '\n';
        });
        out.flush();
    }

    static const char* kindName (mtrace::Ev k) noexcept
    {
        using E = mtrace::Ev;
        switch (k)
        {
            case E::BlockStart:  return "BLK";
            case E::NoteOnLive:  return "NLIV";
            case E::NoteOnGen:   return "NGEN";
            case E::NoteRetrig:  return "NRTG";
            case E::NoteOff:     return "NOFF";
            case E::VoiceSteal:  return "STEAL";
            case E::CC:          return "CC";
            case E::Panic:       return "PANIC";
            case E::AllNotesOff: return "ANOFF";
            case E::LoopRec:     return "LREC";
            case E::LoopEmit:    return "LEMIT";
            case E::LoopWrap:    return "LWRAP";
        }
        return "?";
    }

    juce::String  path;
    std::ofstream out;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiTraceWriter)
};
