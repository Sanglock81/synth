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
    // Ring-size ceiling for ONE audio lane (J2). Sized so audio loops are honest at live
    // tempos: 32 bars * 4 beats * (60 s / 120 BPM) = 64 s. So 32-bar audio records fully down
    // to ~120 BPM and 16-bar down to ~60 BPM; below that the audio lane's selectable bar count
    // is reduced (the processor's honest clamp) so the UI never promises a length the ring can't
    // hold. MIDI lanes keep full length (1..32 bars) at ANY tempo.
    // Total resident audio memory = kMaxLoopSeconds * sampleRate * 2 ch * 4 B * 4 lanes
    // (~98 MB @ 48 kHz — negligible RAM; the ThinkPad deploy constraint is CPU %, not memory).
    static constexpr double kMaxLoopSeconds = 64.0;

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

    // J3 scenes (message thread): snapshot the recorded region into heap buffers, and recall it.
    // Compact: only the actually-recorded `contentLength()` samples are copied, so per-scene audio
    // memory scales with what was recorded, not the full ring.
    void snapshotInto (std::vector<float>& outL, std::vector<float>& outR) const
    {
        const int len = contentLength();
        outL.assign (bufL.begin(), bufL.begin() + len);
        outR.assign (bufR.begin(), bufR.begin() + len);
    }
    void loadFrom (const std::vector<float>& inL, const std::vector<float>& inR)
    {
        std::fill (bufL.begin(), bufL.end(), 0.0f);
        std::fill (bufR.begin(), bufR.end(), 0.0f);
        const int len = (int) std::min ({ inL.size(), inR.size(), (std::size_t) cap });
        for (int i = 0; i < len; ++i) { bufL[(std::size_t) i] = inL[(std::size_t) i]; bufR[(std::size_t) i] = inR[(std::size_t) i]; }
        content = (len > 0);
        pos = 0;
    }

private:
    std::vector<float> bufL, bufR;
    int  cap = 0, loopLen = 1, pos = 0;
    bool rec = false, play = false, content = false;
};
