// ============================================================================
// #96 Unison — the stereo stack voice. Count 1 is the mono path (covered bit-exact
// by the goldens); here we exercise count > 1: real stereo width, level held by the
// 1/sqrt(N) trim, the note-on count LATCH, and click-safety on note-off.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "SynthVoice.h"
#include <vector>
#include <cmath>

namespace
{
    constexpr double kSR = 48000.0;

    VoiceParams sawParams()
    {
        VoiceParams p;
        p.osc1Wave = 0; p.osc1Level = 0.8f; p.osc2Level = 0.0f; p.osc3Level = 0.0f;
        p.cutoffHz = 18000.0f; p.filterEnvAmt = 0.0f; p.resonance = 0.05f;
        p.ampA = 0.001f; p.ampD = 0.001f; p.ampS = 1.0f; p.ampR = 0.05f;
        return p;
    }

    void renderStereo (SynthVoice& v, VoiceParams& p, std::vector<float>& L, std::vector<float>& R, int blocks = 48)
    {
        L.assign ((std::size_t) blocks * 128, 0.0f);
        R.assign ((std::size_t) blocks * 128, 0.0f);
        for (int b = 0; b < blocks; ++b)
            v.renderStereo (L.data() + b * 128, R.data() + b * 128, 128, p);
    }

    double power (const std::vector<float>& L, const std::vector<float>& R)
    {
        double e = 0.0; const std::size_t n = L.size();
        for (std::size_t i = 0; i < n; ++i) e += (double) L[i] * L[i] + (double) R[i] * R[i];
        return e / (double) n;
    }
}

TEST_CASE ("unison: a 7-voice stack is audible, STEREO, and bounded", "[unison]")
{
    SynthVoice v; v.prepare (kSR);
    auto p = sawParams(); p.unisonCount = 7; p.unisonDetune = 0.5f; p.unisonWidth = 0.9f;
    v.noteOn (60, 0.9f, 1, 0, 0, false, 0, 0, 0, /*unison*/ 7);
    REQUIRE (v.isUnison());

    std::vector<float> L, R; renderStereo (v, p, L, R);

    double diff = 0.0, peak = 0.0;
    for (std::size_t i = 0; i < L.size(); ++i)
    {
        diff += std::abs ((double) L[i] - R[i]);
        peak = std::max (peak, std::max (std::abs ((double) L[i]), std::abs ((double) R[i])));
    }
    REQUIRE (power (L, R) > 0.001);        // audible
    REQUIRE (peak < 1.5);                  // bounded — no runaway sum
    REQUIRE (diff > 1.0);                  // the pan spread genuinely decorrelates L and R
}

TEST_CASE ("unison: the 1/sqrt(N) trim keeps the level in the same ballpark (not N x louder)", "[unison]")
{
    // Mono reference (count 1, the render() path) vs a 7-voice stack — total power must stay within a
    // small band, NOT ~7x (correlated) or ~49x. Proves the level trim.
    SynthVoice mono; mono.prepare (kSR);
    auto pm = sawParams();
    mono.noteOn (60, 0.9f, 1, 0, 0, false, 0, 0, 0, /*unison*/ 1);
    std::vector<float> ML ((std::size_t) 48 * 128, 0.0f);
    for (int b = 0; b < 48; ++b) mono.render (ML.data() + b * 128, 128, pm);
    double em = 0.0; for (float s : ML) em += (double) s * s; em = 2.0 * em / (double) ML.size();   // dual-mono power

    SynthVoice uni; uni.prepare (kSR);
    auto pu = sawParams(); pu.unisonCount = 7; pu.unisonDetune = 0.4f; pu.unisonWidth = 0.0f;   // width 0 -> centred, comparable
    uni.noteOn (60, 0.9f, 1, 0, 0, false, 0, 0, 0, 7);
    std::vector<float> UL, UR; renderStereo (uni, pu, UL, UR);
    const double eu = power (UL, UR);

    INFO ("mono power=" << em << " unison power=" << eu);
    REQUIRE (eu > 0.25 * em);
    REQUIRE (eu < 4.0  * em);
}

TEST_CASE ("unison: the count is LATCHED at note-on (no mid-note path switch)", "[unison]")
{
    SynthVoice v; v.prepare (kSR);
    auto p = sawParams(); p.unisonCount = 5;
    v.noteOn (60, 0.9f, 1, 0, 0, false, 0, 0, 0, /*unison*/ 5);
    REQUIRE (v.isUnison());
    // A param edit to count 1 mid-note must NOT flip the voice to the mono path — the latch holds
    // the stereo path for the life of the note (the engine keeps routing it to the stereo bus).
    p.unisonCount = 1;
    std::vector<float> L, R; renderStereo (v, p, L, R, 4);
    REQUIRE (v.isUnison());
}

TEST_CASE ("unison: note-off release is click-free", "[unison][click]")
{
    SynthVoice v; v.prepare (kSR);
    auto p = sawParams(); p.osc1Wave = 3;   // SINE: smooth, so a sample step means an envelope click (not a saw reset)
    p.unisonCount = 7; p.unisonDetune = 0.5f; p.unisonWidth = 0.8f;
    p.ampR = 0.08f;
    v.noteOn (60, 0.9f, 1, 0, 0, false, 0, 0, 0, 7);

    std::vector<float> L ((std::size_t) 128, 0.0f), R ((std::size_t) 128, 0.0f);
    for (int b = 0; b < 20; ++b) { std::fill (L.begin(), L.end(), 0.0f); std::fill (R.begin(), R.end(), 0.0f);
        v.renderStereo (L.data(), R.data(), 128, p); }        // settle
    v.noteOff();
    float prevL = L.back(), prevR = R.back(), maxStep = 0.0f;
    for (int b = 0; b < 40 && v.isActive(); ++b)
    {
        std::fill (L.begin(), L.end(), 0.0f); std::fill (R.begin(), R.end(), 0.0f);
        v.renderStereo (L.data(), R.data(), 128, p);
        for (int i = 0; i < 128; ++i)
        {
            maxStep = std::max (maxStep, std::max (std::abs (L[(std::size_t) i] - prevL),
                                                   std::abs (R[(std::size_t) i] - prevR)));
            prevL = L[(std::size_t) i]; prevR = R[(std::size_t) i];
        }
    }
    INFO ("max sample step over the release = " << maxStep);
    REQUIRE (maxStep < 0.15f);   // a smooth release, no discontinuity
}
