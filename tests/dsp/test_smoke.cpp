// Ported from tests/dsp_smoke_test.cpp: render a C-major chord through the
// full engine and check the output is non-silent, finite, and bounded.
#include <catch2/catch_test_macros.hpp>
#include "SynthEngine.h"
#include "test_util.h"
#include <vector>

TEST_CASE ("engine smoke: C-major chord is non-silent, finite, bounded", "[smoke][engine]")
{
    SynthEngine engine;
    engine.prepare (48000.0);

    VoiceParams p;                 // defaults: saw/saw mix, 2 kHz LP, standard ADSRs
    engine.noteOn (60, 0.8f);
    engine.noteOn (64, 0.8f);
    engine.noteOn (67, 0.8f);

    std::vector<float> out (48000, 0.0f);
    for (int block = 0; block < 48000 / 256; ++block)
    {
        engine.render (out.data() + block * 256, 256, p, 2.0f, 0, 0.0f, 0);
        if (block == 100) { engine.noteOff (60); engine.noteOff (64); engine.noteOff (67); }
    }

    REQUIRE (tu::allFinite (out));
    const float pk = tu::peak (out);
    REQUIRE (pk > 0.01f);
    REQUIRE (pk < 4.0f);
}
