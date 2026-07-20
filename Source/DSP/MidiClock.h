#pragma once
#include <cmath>
#include <cstdlib>

// ============================================================================
// MIDI clock generator (JUCE-free, RT-safe). Turns the transport beat position into a
// stream of 24-ppq clock ticks plus Start/Stop, at SAMPLE-ACCURATE offsets within each block so
// downstream loopers/pedals see minimal jitter.
//
// The owner calls process() once per block with the block-start beat position and samples-per-beat
// (from the J1 transport), and an emit(sampleOffset, kind) callback. Clock ticks are emitted
// whenever `enabled` (so a slave always tracks tempo); Start/Stop fire on the `running` edges.
// A transport jump (host loop/seek) re-aligns the tick phase instead of spraying a burst.
// ============================================================================

class MidiClockGenerator
{
public:
    enum Kind { Clock = 0, Start = 1, Stop = 2 };
    static constexpr double kPpq = 24.0;   // MIDI clock: 24 pulses per quarter note

    void reset() { nextTick = 0; runningPrev = false; primed = false; }

    // emit(int sampleOffset, Kind). `beatsAtBlockStart` is the transport position (quarter notes)
    // at the first sample of the block; `samplesPerBeat` = sr*60/bpm; `numSamples` the block length.
    template <typename Emit>
    void process (double beatsAtBlockStart, double samplesPerBeat, int numSamples,
                  bool enabled, bool running, Emit&& emit)
    {
        if (! enabled)
        {
            if (runningPrev) { emit (0, Stop); runningPrev = false; }
            primed = false;
            return;
        }
        if (samplesPerBeat < 1.0 || numSamples <= 0) return;

        // Phase-lock the tick counter to the transport: prime on (re)enable, and re-align on a jump
        // (a host loop brace / locate moves beatsAtBlockStart discontinuously).
        const long curTick = (long) std::floor (beatsAtBlockStart * kPpq);
        if (! primed || std::labs (nextTick - curTick) > 2) { nextTick = curTick; primed = true; }

        if (running && ! runningPrev) emit (0, Start);
        if (! running && runningPrev) emit (0, Stop);
        runningPrev = running;
        if (! running) return;   // clock ticks only while the transport is running

        // Reference each tick to its ABSOLUTE sample (round once) and subtract the block-start
        // sample — avoids the FP error of a small (tickBeat - blockStart) difference, so the grid is
        // exact at integer tempi and within a sample otherwise.
        const long blockStartSample = std::lround (beatsAtBlockStart * samplesPerBeat);
        const double blockEndBeats = beatsAtBlockStart + (double) numSamples / samplesPerBeat;
        while ((double) nextTick / kPpq < blockEndBeats)
        {
            const double tickBeat = (double) nextTick / kPpq;
            if (tickBeat >= beatsAtBlockStart)
            {
                int off = (int) (std::lround (tickBeat * samplesPerBeat) - blockStartSample);
                off = off < 0 ? 0 : (off >= numSamples ? numSamples - 1 : off);
                emit (off, Clock);
            }
            ++nextTick;
        }
    }

private:
    long nextTick = 0;      // absolute 24-ppq tick index of the next clock pulse
    bool runningPrev = false;
    bool primed = false;
};
