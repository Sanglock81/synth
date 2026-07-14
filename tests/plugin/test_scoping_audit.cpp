// ============================================================================
// FX/filter scoping audit (task #50). Reasserts that ALL per-sound processing belongs to a
// PART and is set via edit focus: two parts given deliberately different sound-design
// settings render distinct, per-part-correct output through the real processor.
//
// Audit result (params categorised against perPartSoundIds):
//   PER-PART (correct): osc*, filter*, *env*, glide, vel*, the whole FX chain
//     (chorus/delay/reverb/width enables + params + ORDER), and all three LFOs.
//   GLOBAL (correct): arp*, chord*, seq*, loop* (generators/fixtures), macro1-8,
//     partN level/pan (mixer), tempo, master_gain, poly_mode (shared engine mode, v1).
//   FINDING: eq* is a MASTER (global) EQ — moved per-part in the EQ task (#51).
// Per-part delay + LFO isolation are covered in tests/dsp/test_multitimbral.cpp; per-part
// width reaching a focused locked part is covered in test_fx_topology.cpp. This adds the
// processor-level FILTER case.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "PluginProcessor.h"
#include <cmath>

namespace
{
    void setVal (VASynthProcessor& p, const char* id, float v)
    { p.apvts.getParameter (id)->setValueNotifyingHost (p.apvts.getParameter (id)->convertTo0to1 (v)); }
    void set01 (VASynthProcessor& p, const char* id, float v) { p.apvts.getParameter (id)->setValueNotifyingHost (v); }

    // High-frequency proxy: sum of squared sample-to-sample deltas over a note on `part`.
    double brightness (VASynthProcessor& p, int part)
    {
        juce::AudioBuffer<float> buf (2, 256); juce::MidiBuffer m;
        p.routeNoteOn (60, 0.9f, part);
        double e = 0.0; float prev = 0.0f;
        for (int b = 0; b < 20; ++b)
        {
            buf.clear(); m.clear(); p.processBlock (buf, m);
            const float* L = buf.getReadPointer (0);
            for (int i = 0; i < 256; ++i) { const float d = L[i] - prev; e += (double) d * d; prev = L[i]; }
        }
        p.routeNoteOff (60, part);
        for (int b = 0; b < 8; ++b) { buf.clear(); m.clear(); p.processBlock (buf, m); }   // let it die
        return e;
    }
}

TEST_CASE ("scoping: two parts with different filter cutoffs render distinct (per-part filter)", "[plugin][scoping][isolation]")
{
    namespace ID = ParamID;
    VASynthProcessor p; p.prepareToPlay (48000.0, 256);

    // Part 1 = BRIGHT saw (high cutoff); part 2 = DARK saw (low cutoff). Configure each via
    // edit focus, then focus back to the live part so both 1 and 2 are baked/locked.
    p.setEditFocus (1);
    set01 (p, ID::osc1On, 1.0f); setVal (p, ID::osc1Wave, 0.0f);   // saw -> harmonics to filter
    set01 (p, ID::osc2On, 0.0f); set01 (p, ID::osc3On, 0.0f);
    setVal (p, ID::filterCutoff, 7000.0f); setVal (p, ID::filterEnvAmt, 0.0f);

    p.setEditFocus (2);
    set01 (p, ID::osc1On, 1.0f); setVal (p, ID::osc1Wave, 0.0f);
    set01 (p, ID::osc2On, 0.0f); set01 (p, ID::osc3On, 0.0f);
    setVal (p, ID::filterCutoff, 220.0f); setVal (p, ID::filterEnvAmt, 0.0f);

    p.setEditFocus (0);                                            // both parts now locked/baked

    const double bright = brightness (p, 1);
    const double dark   = brightness (p, 2);
    INFO ("part1(bright) HF=" << bright << "  part2(dark) HF=" << dark);
    REQUIRE (bright > 0.0);
    REQUIRE (dark > 0.0);
    REQUIRE (bright > dark * 3.0);   // each part used ITS OWN filter cutoff
}
