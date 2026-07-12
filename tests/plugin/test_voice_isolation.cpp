// ============================================================================
// Per-part voice isolation: notes on one part must never be stolen by notes on
// another part. (The seq/arp/looper hammering one part shouldn't cut your live
// playing on another.) Voice stealing is confined to the same part.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "PluginProcessor.h"

namespace
{
    void s01 (VASynthProcessor& p, const char* id, float v) { p.apvts.getParameter (id)->setValueNotifyingHost (v); }
    double energy (VASynthProcessor& p, int blocks)
    {
        juce::AudioBuffer<float> buf (2, 128); juce::MidiBuffer m; double e = 0.0;
        for (int b = 0; b < blocks; ++b) { buf.clear(); p.processBlock (buf, m); e += buf.getRMSLevel (0, 0, 128); }
        return e;
    }
}

TEST_CASE ("voice isolation: a held note on one part survives another part's barrage", "[plugin][voices][isolation]")
{
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    p.setPartPreset (1, "Fat Saw Bass");           // part 1 gets a real (loud) sound
    s01 (p, ParamID::part1Level, 0.0f);            // ...but muted: its voices still ALLOCATE + steal

    // Part 0 (audible): hold a note.
    p.routeNoteOn (60, 0.9f, 0);
    const double e1 = energy (p, 20);
    REQUIRE (e1 > 0.0);                            // part 0 is sounding

    // Part 1: fire many distinct notes -> fills the 16-voice pool and forces steals.
    for (int n = 36; n < 84; ++n) p.routeNoteOn (n, 0.9f, 1);
    const double e2 = energy (p, 20);

    // Part 0's held note must NOT have been stolen by part 1.
    REQUIRE (e2 > 0.5 * e1);
}
