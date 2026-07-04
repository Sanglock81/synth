// Quick offline sanity test: render 1 second of audio through the full
// engine and check output is non-silent, finite, and bounded.
#include "../Source/DSP/SynthEngine.h"
#include <cstdio>
#include <vector>

int main()
{
    SynthEngine engine;
    engine.prepare (48000.0);

    VoiceParams p;   // defaults: saw/saw mix, 2kHz LP, standard ADSRs

    engine.noteOn (60, 0.8f);   // middle C
    engine.noteOn (64, 0.8f);
    engine.noteOn (67, 0.8f);   // C major, why not

    std::vector<float> out (48000, 0.0f);
    float peak = 0.0f;
    bool finite = true;

    for (int block = 0; block < 48000 / 256; ++block)
    {
        engine.render (out.data() + block * 256, 256, p, 2.0f, 0, 0.0f, 0);
        if (block == 100) { engine.noteOff (60); engine.noteOff (64); engine.noteOff (67); }
    }

    for (float s : out)
    {
        if (!std::isfinite (s)) finite = false;
        peak = std::max (peak, std::abs (s));
    }

    printf ("peak = %.3f, finite = %s\n", peak, finite ? "yes" : "NO");
    return (peak > 0.01f && peak < 4.0f && finite) ? 0 : 1;
}
