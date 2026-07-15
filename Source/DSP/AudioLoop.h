#pragma once
#include <vector>
#include <cstddef>
#include <algorithm>

// ============================================================================
// Stereo AUDIO loop — JUCE-free, RT-safe on the audio thread (no alloc/lock there).
//
// The tape complement to the MIDI Looper: it records a stereo signal (the focused
// part's post-FX output, captured by the engine) into a preallocated ring, then
// plays it back summed into the master. Storage is allocated ONCE in prepare()
// (message/prepare thread); recordBlock/playBlock/advance never allocate.
//
// Record is overdub-by-sum (a fresh pass writes onto silence; later passes layer),
// matching the MIDI looper's overdub feel. The owner drives the shared loop clock:
// same loop length + one advance(n) per block keeps this lane sample-locked to the
// MIDI lane (both start at 0 and step identically).
// ============================================================================

class AudioLoop
{
public:
    // Ring-size ceiling for ONE audio lane. Sized for 4 bars of 4 beats at a 40 BPM
    // practical floor:  4 bars * 4 beats * (60 s / 40 BPM) = 24 s. Below 40 BPM the audio
    // lane's selectable bar count is reduced (the processor's honest clamp) so the UI never
    // promises a length the ring can't hold; MIDI lanes keep full length at ANY tempo.
    // Total resident audio memory = kMaxLoopSeconds * sampleRate * 2 ch * 4 B * 4 lanes
    // (~37 MB @ 48 kHz). Raise this one constant for a studio build.
    static constexpr double kMaxLoopSeconds = 24.0;

    // Allocate the ring for the largest loop the transport can ask for. Call from
    // prepareToPlay (allocation is fine there, never on the audio thread).
    void prepare (int maxSamples)
    {
        cap = maxSamples < 1 ? 1 : maxSamples;
        bufL.assign ((std::size_t) cap, 0.0f);
        bufR.assign ((std::size_t) cap, 0.0f);
        loopLen = cap; pos = 0; content = false;
    }

    void setLoopLength (int samples)
    {
        loopLen = samples < 1 ? 1 : (samples > cap ? cap : samples);   // never exceed the ring
        if (pos >= loopLen) pos %= loopLen;
    }
    int  loopLength() const { return loopLen; }
    int  position()   const { return pos; }

    void setRecording (bool b) { rec = b; }
    void setPlaying   (bool b) { play = b; }
    bool recording() const { return rec; }
    bool playing()   const { return play; }
    bool hasContent() const { return content; }

    void clear()
    {
        std::fill (bufL.begin(), bufL.end(), 0.0f);
        std::fill (bufR.begin(), bufR.end(), 0.0f);
        pos = 0; content = false;
    }

    // Overdub this block's input into the ring at the loop position (REC only).
    void recordBlock (const float* inL, const float* inR, int n)
    {
        if (! rec || cap == 0) return;
        for (int i = 0; i < n; ++i)
        {
            const int idx = (pos + i) % loopLen;
            bufL[(std::size_t) idx] += inL[i];
            bufR[(std::size_t) idx] += inR[i];
        }
        content = true;
    }

    // Add this block of loop content into the output (PLAY only).
    void playBlock (float* outL, float* outR, int n) const
    {
        if (! play || ! content || cap == 0) return;
        for (int i = 0; i < n; ++i)
        {
            const int idx = (pos + i) % loopLen;
            outL[i] += bufL[(std::size_t) idx];
            outR[i] += bufR[(std::size_t) idx];
        }
    }

    void advance (int n) { pos = (pos + n) % loopLen; }

    // Export access (message thread): the recorded content is the first loopLen samples.
    int          contentLength() const { return content ? loopLen : 0; }
    const float* dataL() const { return bufL.data(); }
    const float* dataR() const { return bufR.data(); }

private:
    std::vector<float> bufL, bufR;
    int  cap = 0, loopLen = 1, pos = 0;
    bool rec = false, play = false, content = false;
};
