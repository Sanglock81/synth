// ============================================================================
// [6b] Plugin-layer FX: the chain order state-tree property (validation +
// persistence) and the stereo FX path through processBlock.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginProcessor.h"
#include <vector>
#include <cmath>

namespace
{
    constexpr double kSR = 48000.0;
    constexpr int    kBlock = 512;

    // Render `blocks` blocks, holding a note for the first `onBlocks`, and return
    // the concatenated stereo output (L and R separately).
    struct StereoOut { std::vector<float> L, R; };

    StereoOut renderNote (VASynthProcessor& p, int blocks, int onBlocks, int note = 60)
    {
        p.prepareToPlay (kSR, kBlock);
        StereoOut out;
        for (int b = 0; b < blocks; ++b)
        {
            juce::AudioBuffer<float> buf (2, kBlock);
            buf.clear();
            juce::MidiBuffer midi;
            if (b == 0)         midi.addEvent (juce::MidiMessage::noteOn  (1, note, 0.9f), 0);
            if (b == onBlocks)  midi.addEvent (juce::MidiMessage::noteOff (1, note), 0);
            p.processBlock (buf, midi);
            const float* l = buf.getReadPointer (0);
            const float* r = buf.getReadPointer (1);
            for (int i = 0; i < kBlock; ++i) { out.L.push_back (l[i]); out.R.push_back (r[i]); }
        }
        return out;
    }

    double rms (const std::vector<float>& x, int start, int len)
    {
        double a = 0.0; for (int i = 0; i < len; ++i) { const double s = x[(size_t)(start + i)]; a += s * s; }
        return std::sqrt (a / len);
    }
    double rmsDiff (const std::vector<float>& a, const std::vector<float>& b, int start, int len)
    {
        double acc = 0.0; for (int i = 0; i < len; ++i) { const double d = a[(size_t)(start+i)] - b[(size_t)(start+i)]; acc += d*d; }
        return std::sqrt (acc / len);
    }
}

TEST_CASE ("fx_order: setFxOrder round-trips a permutation and rejects invalid input", "[plugin][6b][fxorder]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;

    int def[5]; p.getFxOrder (def);             // 5 blocks now (chorus/delay/reverb/width/EQ)
    for (int i = 0; i < 5; ++i) REQUIRE (def[i] == i);

    const int good[5] { 4, 3, 2, 1, 0 };
    p.setFxOrder (good);
    int got[5]; p.getFxOrder (got);
    for (int i = 0; i < 5; ++i) REQUIRE (got[i] == good[i]);

    const int dup[5] { 0, 0, 1, 2, 3 };         // not a permutation -> ignored
    p.setFxOrder (dup);
    p.getFxOrder (got);
    for (int i = 0; i < 5; ++i) REQUIRE (got[i] == good[i]);   // unchanged

    const int oor[5] { 0, 1, 2, 3, 9 };         // out of range -> ignored
    p.setFxOrder (oor);
    p.getFxOrder (got);
    for (int i = 0; i < 5; ++i) REQUIRE (got[i] == good[i]);
}

TEST_CASE ("fx_order persists through save/load", "[plugin][6b][fxorder][state]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor src;
    const int ord[5] { 2, 0, 4, 3, 1 };
    src.setFxOrder (ord);

    juce::MemoryBlock blob;
    src.getStateInformation (blob);

    VASynthProcessor dst;
    dst.setStateInformation (blob.getData(), (int) blob.getSize());
    int got[5]; dst.getFxOrder (got);
    for (int i = 0; i < 5; ++i) REQUIRE (got[i] == ord[i]);
}

TEST_CASE ("enabling reverb adds a tail that outlives the dry note", "[plugin][6b][fx][audio]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    auto tailRms = [] (bool reverbOn)
    {
        VASynthProcessor p;
        p.apvts.getParameter ("amp_release")->setValueNotifyingHost (0.0f);   // snappy dry decay
        if (reverbOn)
        {
            p.apvts.getParameter ("fx_reverb_on")->setValueNotifyingHost (1.0f);
            p.apvts.getParameter ("reverb_mix")->setValueNotifyingHost (0.8f);
            p.apvts.getParameter ("reverb_size")->setValueNotifyingHost (0.9f);
        }
        auto out = renderNote (p, 60, 6);        // note off at block 6, keep rendering
        return rms (out.L, kBlock * 40, kBlock * 8);   // well after the note released
    };

    const double dry = tailRms (false);
    const double wet = tailRms (true);
    REQUIRE (wet > dry * 5.0);                    // the reverb tail is clearly audible
    REQUIRE (dry < 0.02);                         // ...and the dry path really has decayed
}

TEST_CASE ("chorus makes the output genuinely stereo", "[plugin][6b][fx][stereo]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    auto stereoSpread = [] (bool chorusOn)
    {
        VASynthProcessor p;
        p.apvts.getParameter ("amp_sustain")->setValueNotifyingHost (1.0f);
        if (chorusOn)
        {
            p.apvts.getParameter ("fx_chorus_on")->setValueNotifyingHost (1.0f);
            p.apvts.getParameter ("chorus_mix")->setValueNotifyingHost (0.8f);
            p.apvts.getParameter ("chorus_depth")->setValueNotifyingHost (1.0f);
        }
        auto out = renderNote (p, 40, 100);      // hold the whole time
        return rmsDiff (out.L, out.R, kBlock * 20, kBlock * 8);
    };

    REQUIRE (stereoSpread (false) < 1e-6);        // dry synth is mono (L == R)
    REQUIRE (stereoSpread (true)  > 0.01);        // chorus decorrelates into stereo
}
