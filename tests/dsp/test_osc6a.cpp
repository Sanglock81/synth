// ============================================================================
// 6A engine: OSC3 parity, per-source level model, kill-switch (silent + cheaper),
// velocity->amp / velocity->cutoff, click-free toggling. Engine-level, JUCE-free.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "SynthEngine.h"
#include "test_util.h"
#include <vector>
#include <chrono>
#include <cstring>

namespace
{
    constexpr double kSR = 48000.0;

    std::vector<float> render1 (VoiceParams p, int note, float vel, int n)
    {
        SynthEngine e; e.prepare (kSR);
        e.noteOn (note, vel);
        std::vector<float> out (n, 0.0f);
        e.render (out.data(), n, p, 2.0f, 0, 0.0f, 0);
        return out;
    }
}

TEST_CASE ("OSC3 is bit-identical to OSC1 given the same settings", "[6a][osc3][parity]")
{
    VoiceParams a; a.osc1Wave = 0; a.osc1Level = 0.8f; a.osc2Level = 0; a.osc3Level = 0;
    VoiceParams b; b.osc3Wave = 0; b.osc3Level = 0.8f; b.osc1Level = 0; b.osc2Level = 0;
    a.cutoffHz = b.cutoffHz = 18000.0f; a.resonance = b.resonance = 0.0f;

    auto oa = render1 (a, 57, 1.0f, 8192);   // osc1 only
    auto ob = render1 (b, 57, 1.0f, 8192);   // osc3 only, same wave/level
    REQUIRE (std::memcmp (oa.data(), ob.data(), oa.size() * sizeof (float)) == 0);
}

TEST_CASE ("a source at level 0 is silent", "[6a][level]")
{
    VoiceParams p; p.osc1Level = 0.0f; p.osc2Level = 0.0f; p.osc3Level = 0.0f; p.noiseLevel = 0.0f;
    auto out = render1 (p, 60, 1.0f, 8192);
    REQUIRE (tu::peak (out) < 1.0e-6f);
}

TEST_CASE ("kill switch: off == level 0, and skips oscillator work (cheaper)", "[6a][kill]")
{
    // 'Off' is folded to level 0 by the processor, so at the engine level off IS
    // level 0 -> bit-identical (covered by the level test). Here we prove the
    // voice actually SKIPS work for silent sources: 1 osc on renders faster than 3.
    auto timeCfg = [] (int oscsOn)
    {
        SynthEngine e; e.prepare (kSR);
        VoiceParams p; p.osc1Wave = p.osc2Wave = p.osc3Wave = 0;   // saws
        p.osc1Level = 0.8f;
        p.osc2Level = oscsOn >= 2 ? 0.8f : 0.0f;
        p.osc3Level = oscsOn >= 3 ? 0.8f : 0.0f;
        p.cutoffHz = 2000.0f; p.filterEnvAmt = 0.3f;
        for (int i = 0; i < 12; ++i) e.noteOn (40 + i, 0.8f);
        std::vector<float> out (256, 0.0f);
        for (int i = 0; i < 50; ++i) e.render (out.data(), 256, p, 2.0f, 0, 0.0f, 0);   // warm
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 2000; ++i) e.render (out.data(), 256, p, 2.0f, 0, 0.0f, 0);
        return std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
    };
    const double t1 = timeCfg (1), t3 = timeCfg (3);
    INFO ("1-osc=" << t1 << "s  3-osc=" << t3 << "s");
    REQUIRE (t1 < t3 * 0.8);        // skipping 2 of 3 oscillators is clearly cheaper
}

TEST_CASE ("velocity -> amp: scale = (1-v2a) + v2a*velocity", "[6a][velocity][amp]")
{
    auto rmsAtVel = [] (float vel)
    {
        VoiceParams p; p.osc1Wave = 3; p.osc1Level = 0.8f; p.osc2Level = 0; p.osc3Level = 0;
        p.cutoffHz = 18000.0f; p.filterEnvAmt = 0; p.velToAmp = 0.7f;
        p.ampA = 0.001f; p.ampD = 0.001f; p.ampS = 1.0f;
        auto out = render1 (p, 57, vel, 8192);
        auto steady = tu::slice (out, 2048, out.size() - 2048);
        return tu::rms (steady);
    };
    const double r0 = rmsAtVel (0.0f), r5 = rmsAtVel (0.5f), r8 = rmsAtVel (0.8f), r1 = rmsAtVel (1.0f);
    // scale(v) = 0.3 + 0.7*v -> 0.3, 0.65, 0.86, 1.0. Compare ratios to vel=1.
    REQUIRE (r0 / r1 == Catch::Approx (0.30).margin (0.02));
    REQUIRE (r5 / r1 == Catch::Approx (0.65).margin (0.02));
    REQUIRE (r8 / r1 == Catch::Approx (0.86).margin (0.02));
}

TEST_CASE ("velocity -> cutoff: higher velocity opens the filter", "[6a][velocity][cutoff]")
{
    // A LP on a saw: more velocity -> more HIGH-frequency energy through the
    // filter. Total RMS is dominated by the fundamental, so measure the high band.
    auto highBandEnergy = [] (float vel)
    {
        VoiceParams p; p.osc1Wave = 0; p.osc1Level = 0.8f; p.osc2Level = 0; p.osc3Level = 0;
        p.cutoffHz = 400.0f; p.resonance = 0.2f; p.filterEnvAmt = 0.0f;
        p.velToCutoff = 1.0f;                       // up to +3 oct
        p.ampA = 0.001f; p.ampS = 1.0f; p.velToAmp = 0.0f;   // remove amp effect
        auto out = render1 (p, 45, vel, 1 << 15);
        auto seg = tu::slice (out, 8192, (std::size_t) (1 << 14));
        auto mag = tu::magnitudeSpectrum (seg);
        const double binHz = kSR / seg.size();
        double e = 0.0;
        for (std::size_t b = 0; b < mag.size(); ++b)
            if (b * binHz > 1000.0) e += mag[b] * mag[b];
        return e;
    };
    const double lo = highBandEnergy (0.1f), hi = highBandEnergy (1.0f);
    INFO ("high-band energy lo=" << lo << " hi=" << hi << " ratio=" << hi / lo);
    REQUIRE (hi > lo * 3.0);                          // velocity clearly opens the filter
}

TEST_CASE ("toggling an oscillator on mid-note does not click", "[6a][kill][click]")
{
    SynthEngine e; e.prepare (kSR);
    VoiceParams p; p.osc1Wave = 3; p.osc3Wave = 3; p.osc1Level = 0.5f; p.osc2Level = 0.0f;
    p.osc3Level = 0.0f; p.cutoffHz = 18000.0f; p.filterEnvAmt = 0; p.ampA = 0.002f; p.ampS = 1.0f;
    e.noteOn (57, 0.8f);

    const int block = 64;
    std::vector<float> out (block * 400, 0.0f);
    const int toggleBlock = 200;
    for (int b = 0; b < 400; ++b)
    {
        VoiceParams q = p;
        if (b >= toggleBlock) q.osc3Level = 0.6f;    // osc3 kicks in (smoothed)
        e.render (out.data() + b * block, block, q, 2.0f, 0, 0.0f, 0);
    }
    const int at = block * toggleBlock;
    auto preW = tu::slice (out, (std::size_t)(at - 2048), 2048 - 64);
    auto atW  = tu::slice (out, (std::size_t)(at - 8), 8 + 512);
    const float pre  = tu::maxDelta (preW);
    const float atEv = tu::maxDelta (atW);
    INFO ("pre=" << pre << " at-toggle=" << atEv);
    REQUIRE (tu::allFinite (out));
    REQUIRE (atEv <= pre + 0.05f);                   // no click when the osc fades in
}
