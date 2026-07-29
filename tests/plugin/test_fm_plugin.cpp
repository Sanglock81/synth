// ============================================================================
// #132 Osc FM chain — processor-level behaviour: the headline velocity -> FM depth
// route brightens harder notes; a mid-note FM-depth sweep is click-free (the depth is
// smoothed on the live part); and processBlock stays allocation-free with FM live.
// (Sideband math, keytrack, carrier restriction, goldens: tests/dsp/test_fm.cpp.)
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "PluginProcessor.h"
#include "DSP/ModMatrix.h"
#include "test_util.h"
#include "alloc_hook.h"
#include <cmath>
#include <vector>

namespace
{
    void set01 (VASynthProcessor& p, const char* id, float v) { if (auto* q = p.apvts.getParameter (id)) q->setValueNotifyingHost (v); }
    void setVal (VASynthProcessor& p, const char* id, float v){ if (auto* q = p.apvts.getParameter (id)) q->setValueNotifyingHost (q->convertTo0to1 (v)); }

    // A clean 2-op FM patch: sine carrier (osc1), sine modulator (osc2) at level 0, ratio 1:1,
    // filter wide open, velocity NOT routed to amp/cutoff — so brightness changes come from FM only.
    void fmProbePatch (VASynthProcessor& p)
    {
        namespace ID = ParamID;
        for (auto* rp : p.getParameters()) rp->setValueNotifyingHost (rp->getDefaultValue());
        setVal (p, ID::osc1Wave, 3.0f);              // Sine carrier
        set01 (p, ID::osc1On, 1.0f); setVal (p, ID::osc1Level, 0.85f);
        set01 (p, ID::osc2On, 1.0f); setVal (p, ID::osc2Wave, 3.0f); setVal (p, ID::osc2Level, 0.0f);
        set01 (p, ID::osc3On, 0.0f);
        setVal (p, ID::filterCutoff, 20000.0f); setVal (p, ID::filterReso, 0.0f);
        setVal (p, ID::filterEnvAmt, 0.0f); setVal (p, ID::filterKeytrack, 0.0f);
        setVal (p, ID::velToCutoff, 0.0f); setVal (p, ID::velToAmp, 0.0f);
        setVal (p, ID::ampAttack, 0.002f); setVal (p, ID::ampDecay, 0.02f);
        setVal (p, ID::ampSustain, 1.0f); setVal (p, ID::ampRelease, 0.1f);
    }

    std::vector<float> renderNote (VASynthProcessor& p, float vel)
    {
        p.prepareToPlay (48000.0, 512);
        std::vector<float> mono; mono.reserve (20000);
        for (int done = 0; done < 20000; done += 512)
        {
            juce::AudioBuffer<float> buf (2, 512); buf.clear();
            juce::MidiBuffer m; if (done == 0) m.addEvent (juce::MidiMessage::noteOn (1, 57, vel), 0);
            p.processBlock (buf, m);
            const float* L = buf.getReadPointer (0);
            for (int i = 0; i < 512; ++i) mono.push_back (L[i]);
        }
        return tu::slice (mono, 8192, 8192);
    }

    double centroidHz (const std::vector<float>& x)
    {
        auto mag = tu::magnitudeSpectrum (x);
        const double binHz = 48000.0 / double (x.size());
        double num = 0.0, den = 0.0;
        for (std::size_t k = 1; k < mag.size(); ++k) { num += double (k) * binHz * mag[k]; den += mag[k]; }
        return den > 0.0 ? num / den : 0.0;
    }
}

TEST_CASE ("FM: a Velocity->Osc1Fm route brightens harder notes (the headline)", "[plugin][fm]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    fmProbePatch (p);
    p.setModSlot (-1, 0, ModMatrix::Velocity, ModMatrix::Osc1Fm, 0.9f);   // velocity drives FM depth

    const double soft = centroidHz (renderNote (p, 0.2f));
    const double hard = centroidHz (renderNote (p, 1.0f));
    INFO ("centroid soft=" << soft << " hard=" << hard);
    REQUIRE (hard > soft * 1.3);   // harder = more FM index = brighter
}

TEST_CASE ("FM: sweeping the depth mid-note is click-free (smoothed on the live part)", "[plugin][fm][torture]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    fmProbePatch (p);
    p.prepareToPlay (48000.0, 256);

    float prev = 0.0f, maxJump = 0.0f, peak = 0.0f; bool finite = true;
    auto* fm = p.apvts.getParameter (ParamID::osc1Fm);
    for (int b = 0; b < 400; ++b)
    {
        // Sweep FM depth up and down across the whole range while a note sustains — incl. abrupt jumps.
        const float d = (b % 40 < 20) ? (float) (b % 20) / 19.0f : 1.0f - (float) (b % 20) / 19.0f;
        fm->setValueNotifyingHost (d);
        juce::AudioBuffer<float> buf (2, 256); buf.clear();
        juce::MidiBuffer m; if (b == 0) m.addEvent (juce::MidiMessage::noteOn (1, 57, 0.9f), 0);
        p.processBlock (buf, m);
        const float* L = buf.getReadPointer (0);
        for (int i = 0; i < 256; ++i)
        {
            finite = finite && std::isfinite (L[i]);
            peak = std::max (peak, std::abs (L[i]));
            maxJump = std::max (maxJump, std::abs (L[i] - prev));
            prev = L[i];
        }
    }
    INFO ("peak=" << peak << " maxJump=" << maxJump);
    REQUIRE (finite);
    REQUIRE (peak <= 1.0001f);      // safety clipper holds
    REQUIRE (maxJump < 0.35f);      // no click (same threshold as the click-torture suite)
}

TEST_CASE ("FM: processBlock stays allocation-free with FM live", "[plugin][fm][rt][alloc]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    fmProbePatch (p);
    setVal (p, ParamID::osc1Fm, 0.5f);
    p.setModSlot (-1, 0, ModMatrix::Velocity, ModMatrix::Osc1Fm, 0.7f);
    p.prepareToPlay (48000.0, 512);

    juce::AudioBuffer<float> buf (2, 512);
    { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOn (1, 57, 0.9f), 0); buf.clear(); p.processBlock (buf, m); }
    for (int i = 0; i < 20; ++i) { buf.clear(); juce::MidiBuffer m; p.processBlock (buf, m); }

    std::size_t news = 0;
    { alloc_hook::AllocGuard g;
      for (int b = 0; b < 200; ++b) { buf.clear(); juce::MidiBuffer m; p.processBlock (buf, m); }
      news = g.count(); }
    INFO ("allocations during FM render: " << news);
    REQUIRE (news == 0);
}
