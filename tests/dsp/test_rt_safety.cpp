// ============================================================================
// Real-time safety: SynthEngine::render must not touch the heap. Renders 1000
// blocks (with active voices, note-ons that trigger stealing, and note-offs)
// inside an armed allocation guard and asserts zero new/delete.
//
// NOTE: this covers the JUCE-free engine. The processor-layer RT hazards
// (MidiLearnManager std::map, monoScratch resize) are addressed in Phase 3 with
// a JUCE-side allocation test.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "SynthEngine.h"
#include "alloc_hook.h"
#include <vector>

TEST_CASE ("SynthEngine::render performs zero heap allocations over 1000 blocks",
           "[rt][alloc]")
{
    SynthEngine engine;
    engine.prepare (48000.0);

    VoiceParams p;                                 // saw/saw, defaults
    const int blockSize = 256;
    std::vector<float> out (blockSize, 0.0f);      // preallocated

    // Pre-touch a note so voices are active before we arm the guard.
    engine.noteOn (60, 0.8f);

    // Snapshot the counts INSIDE the guard scope, then assert OUTSIDE it — the
    // Catch2 REQUIRE/INFO macros themselves allocate, and must not be counted.
    std::size_t news = 0, frees = 0;
    {
        alloc_hook::AllocGuard guard;              // arms counting, zeroes it

        for (int b = 0; b < 1000; ++b)
        {
            // Exercise note lifecycle incl. stealing, all inside the guard.
            if (b % 50 == 0)  engine.noteOn (36 + (b % 40), 0.7f);   // fills + steals
            if (b % 73 == 0)  engine.noteOff (36 + ((b - 1) % 40));

            for (float& s : out) s = 0.0f;
            engine.render (out.data(), blockSize, p, 2.0f + 0.001f * b,
                           b % 4, 0.5f, (b % 3) + 1);
        }

        news  = guard.count();
        frees = guard.frees();
    }                                              // counting disarmed here

    INFO ("new=" << news << " delete=" << frees);
    REQUIRE (news  == 0);
    REQUIRE (frees == 0);
}
