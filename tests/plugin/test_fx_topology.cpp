// PROBE (temporary): do the master EQ and per-part width actually affect the processor's
// output in the real topology? Answers "do they work / did they ever" empirically.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginProcessor.h"
#include <cmath>

namespace
{
    void setV (VASynthProcessor& p, const char* id, float v)
    { p.apvts.getParameter (id)->setValueNotifyingHost (p.apvts.getParameter (id)->convertTo0to1 (v)); }
    void set01 (VASynthProcessor& p, const char* id, float v) { p.apvts.getParameter (id)->setValueNotifyingHost (v); }

    struct Meas { double rms = 0.0, corr = 0.0; };
    // Play a held note on the live part and measure L RMS + L/R correlation over the tail.
    Meas render (VASynthProcessor& p, int note)
    {
        juce::MidiBuffer m; juce::AudioBuffer<float> buf (2, 512);
        p.routeNoteOn (note, 0.9f, 0);
        double sL = 0, sLL = 0, sRR = 0, sLR = 0; int n = 0;
        for (int b = 0; b < 40; ++b)
        {
            buf.clear(); m.clear(); p.processBlock (buf, m);
            const float* L = buf.getReadPointer (0); const float* R = buf.getReadPointer (1);
            for (int i = 0; i < 512; ++i) { sL += L[i]*L[i]; sLL += L[i]*L[i]; sRR += R[i]*R[i]; sLR += L[i]*R[i]; ++n; }
        }
        p.routeNoteOff (note, 0);
        Meas r; r.rms = std::sqrt (sL / n);
        r.corr = (sLL > 1e-12 && sRR > 1e-12) ? sLR / std::sqrt (sLL * sRR) : 1.0;
        return r;
    }
}

TEST_CASE ("per-part EQ affects the processor output (K1: end-of-part, fixed last)", "[plugin][fx][topology]")
{
    VASynthProcessor p; p.prepareToPlay (48000.0, 512);
    p.loadInitPreset();                 // dry mono sine on the live part
    const double flat = render (p, 45).rms;

    set01 (p, ParamID::peqOn, 1.0f);    // enable the per-part EQ, boost every band hard
    setV (p, ParamID::peqB1Gain, 18.0f);
    setV (p, ParamID::peqB2Gain, 18.0f);
    setV (p, ParamID::peqB3Gain, 18.0f);
    setV (p, ParamID::peqB4Gain, 18.0f);
    const double boosted = render (p, 45).rms;

    INFO ("flat rms=" << flat << "  EQ-boosted rms=" << boosted);
    REQUIRE (boosted > flat * 1.5);     // a big broadband boost must raise the level
}

// (The "retired master EQ is inert" test was removed with the eq_* params themselves,
//  pre-1.0 — a removed parameter cannot be set, so the check is moot.)

TEST_CASE ("stereo width affects L/R correlation with a stereo source", "[plugin][fx][topology]")
{
    VASynthProcessor p; p.prepareToPlay (48000.0, 512);
    p.loadInitPreset();
    // Turn on chorus (decorrelates L/R) + width, so width has a side signal to act on.
    set01 (p, ParamID::fxChorusOn, 1.0f);
    set01 (p, ParamID::fxWidthOn, 1.0f);

    setV (p, ParamID::stereoWidth, 0.0f);   // collapse to mono
    const double narrow = render (p, 57).corr;
    setV (p, ParamID::stereoWidth, 2.0f);   // maximally wide
    const double wide = render (p, 57).corr;

    INFO ("width=0 corr=" << narrow << "  width=2 corr=" << wide);
    REQUIRE (narrow > wide + 0.05);          // wider => lower L/R correlation
}

TEST_CASE ("stereo width>1 widens a dry MONO part (allpass decorrelation)", "[plugin][fx][topology]")
{
    VASynthProcessor p; p.prepareToPlay (48000.0, 512);
    p.loadInitPreset();
    set01 (p, ParamID::fxChorusOn, 0.0f); set01 (p, ParamID::fxDelayOn, 0.0f);
    set01 (p, ParamID::fxReverbOn, 0.0f);
    set01 (p, ParamID::fxWidthOn, 1.0f);

    setV (p, ParamID::stereoWidth, 1.0f);
    const double unity = render (p, 57).corr;
    setV (p, ParamID::stereoWidth, 2.0f);
    const double wide = render (p, 57).corr;
    INFO ("dry mono: width=1 corr=" << unity << "  width=2 corr=" << wide);
    REQUIRE (unity > 0.99);                  // unity leaves a mono source mono
    REQUIRE (wide < unity - 0.05);           // width>1 synthesizes side -> genuinely stereo
}

TEST_CASE ("width edited on a FOCUSED LOCKED part reaches that part's audio", "[plugin][fx][topology]")
{
    VASynthProcessor p; p.prepareToPlay (48000.0, 512);
    p.setPartPreset (1, "Fat Saw Bass");     // a locked synth part
    p.setEditFocus (1);                      // focus it -> panel edits + notes route to part 1
    set01 (p, ParamID::fxChorusOn, 1.0f);    // stereo source
    set01 (p, ParamID::fxWidthOn, 1.0f);

    setV (p, ParamID::stereoWidth, 0.0f);
    const double narrow = render (p, 45).corr;
    setV (p, ParamID::stereoWidth, 2.0f);
    const double wide = render (p, 45).corr;
    INFO ("focused locked part width=0 corr=" << narrow << "  width=2 corr=" << wide);
    REQUIRE (narrow > wide + 0.05);          // editing width on the focused part DOES reach its audio
}
