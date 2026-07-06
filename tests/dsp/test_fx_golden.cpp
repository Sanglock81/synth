// ============================================================================
// [6b] FX golden renders. A fixed synthetic input (enveloped 220 Hz sine with a
// sharp onset for the delay/reverb to grab) is run through the FXChain in three
// configurations — bypassed, a default-ish wet patch, and a reordered
// non-default patch — and compared against committed stereo reference WAVs.
// Catches accidental changes to any effect or to the chain/crossfade logic.
//
// Deterministic: the effects are pure math from a reset state, no RNG. Stereo
// output is interleaved (L,R,L,R,...) into the mono WAV container.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "FXChain.h"
#include "test_util.h"
#include <vector>
#include <string>
#include <cmath>

namespace
{
    constexpr double kSR = 48000.0;
    constexpr int    kBlock = 128;
    constexpr int    kLen = (int) (kSR * 0.5);      // 0.5 s

    // Fixed, RNG-free input: a decaying 220 Hz sine with a hard onset.
    std::vector<float> makeInput()
    {
        std::vector<float> x ((size_t) kLen);
        for (int i = 0; i < kLen; ++i)
        {
            const double t = i / kSR;
            const double env = std::exp (-3.0 * t);
            x[(size_t) i] = (float) (0.5 * std::sin (tu::kTwoPi * 220.0 * t) * env);
        }
        x[0] += 0.5f;                                // extra transient
        return x;
    }

    // Render the mono input through the chain (mono duplicated to L/R) and return
    // interleaved stereo.
    std::vector<float> renderFX (const FXParams& p)
    {
        const auto in = makeInput();
        std::vector<float> L (in.begin(), in.end()), R (in.begin(), in.end());

        FXChain chain; chain.prepare (kSR, kBlock);
        chain.setParams (p);
        for (int i = 0; i < kLen; i += kBlock)
        {
            const int n = std::min (kBlock, kLen - i);
            chain.process (L.data() + i, R.data() + i, n);
        }

        std::vector<float> inter ((size_t) kLen * 2);
        for (int i = 0; i < kLen; ++i) { inter[(size_t)(2*i)] = L[(size_t) i]; inter[(size_t)(2*i+1)] = R[(size_t) i]; }
        return inter;
    }

    void checkGolden (const std::string& name, const std::vector<float>& rendered)
    {
        const std::string path = std::string (VASYNTH_GOLDEN_DIR) + "/" + name;
        REQUIRE (tu::allFinite (rendered));

        auto ref = tu::readWavF32 (path);
        if (ref.empty())
        {
            tu::writeWavF32 (path, rendered, int (kSR));
            WARN ("golden reference did not exist; wrote " << path << " — commit it and re-run");
            SUCCEED();
            return;
        }
        REQUIRE (ref.size() == rendered.size());

        double diffSq = 0.0, sigSq = 0.0; float maxAbs = 0.0f;
        for (std::size_t i = 0; i < ref.size(); ++i)
        {
            const double d = double (rendered[i]) - double (ref[i]);
            diffSq += d * d; sigSq += double (ref[i]) * double (ref[i]);
            maxAbs = std::max (maxAbs, std::abs (float (d)));
        }
        const double relDb = tu::linToDb (std::sqrt (diffSq / (double) ref.size())
                                          / std::max (std::sqrt (sigSq / (double) ref.size()), 1e-12));
        INFO (name << " maxAbsDiff=" << maxAbs << " relDiff=" << relDb << " dB");
        REQUIRE (maxAbs < 5.0e-3f);
        REQUIRE (relDb  < -60.0);
    }
}

TEST_CASE ("FX golden: bypassed chain is identity", "[6b][fx][golden]")
{
    FXParams p;                                     // all disabled
    auto out = renderFX (p);
    // Bypassed => interleaved input; verify the L channel matches the source.
    const auto in = makeInput();
    for (int i = 0; i < 8; ++i) REQUIRE (out[(size_t)(2*i)] == in[(size_t) i]);
    checkGolden ("fx_off.f32.wav", out);
}

TEST_CASE ("FX golden: default wet patch (chorus + reverb, natural order)", "[6b][fx][golden]")
{
    FXParams p;
    p.enabled[FXChain::Chorus_] = true;
    p.enabled[FXChain::Reverb_] = true;
    // default params; default order 0,1,2,3
    checkGolden ("fx_default.f32.wav", renderFX (p));
}

TEST_CASE ("FX golden: non-default reordered patch (delay->width->reverb, tuned)", "[6b][fx][golden]")
{
    FXParams p;
    p.enabled[FXChain::Delay_]  = true;
    p.enabled[FXChain::Reverb_] = true;
    p.enabled[FXChain::Width_]  = true;
    p.delayTimeMs = 180.0f; p.delayFeedback = 0.5f; p.delayMix = 0.4f; p.delaySpread = 1.0f;
    p.reverbSize = 0.8f; p.reverbDamp = 0.3f; p.reverbMix = 0.4f;
    p.width = 1.7f;
    p.order[0] = FXChain::Delay_;  p.order[1] = FXChain::Width_;
    p.order[2] = FXChain::Reverb_; p.order[3] = FXChain::Chorus_;
    checkGolden ("fx_nondefault.f32.wav", renderFX (p));
}
