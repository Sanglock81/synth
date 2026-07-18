// ============================================================================
// The per-part EQ must shape a KIT part too (part-bus EQ). Regression for the v1
// "kits are dry" gap where the whole FX chain — including the consolidated EQ — was
// bypassed for kit parts, so EQ'ing a drum part did nothing. The creative FX stay
// dry on kits; only the EQ passes through (eqOnly()).
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>
#include <memory>

#ifndef VASYNTH_DOCS_DIR
 #define VASYNTH_DOCS_DIR "."
#endif

namespace
{
    void setV (VASynthProcessor& p, const char* id, float v)
    { auto* pr = p.apvts.getParameter (id); pr->setValueNotifyingHost (pr->convertTo0to1 (v)); }
    void set01 (VASynthProcessor& p, const char* id, float v) { p.apvts.getParameter (id)->setValueNotifyingHost (v); }

    // Total energy of the kick pad on part 3, rendered fresh (no cross-render state).
    double kickEnergy (bool eqOn)
    {
        VASynthProcessor p; p.prepareToPlay (48000.0, 256);
        p.setPartKit (3, VASynthProcessor::factoryKit ("808 Basics"));   // P4 = kit
        REQUIRE (p.isPartKit (3));
        p.setEditFocus (3);                                              // focus it -> panel EQ edits this part

        if (eqOn)
        {
            set01 (p, ParamID::peqOn, 1.0f);
            setV  (p, ParamID::peqB1Freq, 80.0f);      // sit the low band on the kick fundamental
            setV  (p, ParamID::peqB1Q,   0.7f);
            setV  (p, ParamID::peqB1Gain, 18.0f);      // big low boost
        }

        juce::AudioBuffer<float> buf (2, 256); juce::MidiBuffer m;
        p.routeNoteOn (36, 1.0f, 3);                    // kick pad
        double e = 0.0;
        for (int b = 0; b < 40; ++b)
        {
            buf.clear(); m.clear(); p.processBlock (buf, m);
            const float* L = buf.getReadPointer (0);
            for (int i = 0; i < 256; ++i) e += (double) L[i] * L[i];
        }
        return e;
    }
}

TEST_CASE ("per-part EQ shapes a KIT part (not bypassed like the creative FX)", "[plugin][eq][kit]")
{
    const double dry     = kickEnergy (false);
    const double boosted = kickEnergy (true);
    INFO ("kit kick energy: dry=" << dry << "  low-boosted=" << boosted);
    REQUIRE (dry > 0.0);
    REQUIRE (boosted > dry * 1.3);      // the part EQ audibly lifts the kit's low end
}

TEST_CASE ("kit EQ enabled-but-flat is transparent on the kit", "[plugin][eq][kit]")
{
    // A flat 5-band EQ (all 0 dB) is bit-exact unity in steady state. The ONLY divergence is
    // the FXChain's one-time enable crossfade (~30 ms) at note onset — a toggle-only artifact.
    // Play a warm-up kick to fire + settle that crossfade, let it decay, THEN compare a fresh
    // kick: b's chain is now steadily EQ-on (flat) so it matches a (EQ-off) sample-for-sample.
    VASynthProcessor a; a.prepareToPlay (48000.0, 256);
    a.setPartKit (3, VASynthProcessor::factoryKit ("808 Basics")); a.setEditFocus (3);
    VASynthProcessor b; b.prepareToPlay (48000.0, 256);
    b.setPartKit (3, VASynthProcessor::factoryKit ("808 Basics")); b.setEditFocus (3);
    set01 (b, ParamID::peqOn, 1.0f);                       // section on, all bands flat

    juce::AudioBuffer<float> ba (2, 256), bb (2, 256);
    auto warm = [&] (VASynthProcessor& p) {
        juce::MidiBuffer m; p.routeNoteOn (36, 1.0f, 3);
        for (int blk = 0; blk < 60; ++blk) { juce::AudioBuffer<float> buf (2, 256); buf.clear(); juce::MidiBuffer mm; p.processBlock (buf, mm); }
        p.routeNoteOff (36, 3);
        for (int blk = 0; blk < 20; ++blk) { juce::AudioBuffer<float> buf (2, 256); buf.clear(); juce::MidiBuffer mm; p.processBlock (buf, mm); }
    };
    warm (a); warm (b);                                    // crossfade now fully settled in b

    juce::MidiBuffer m;
    a.routeNoteOn (36, 1.0f, 3); b.routeNoteOn (36, 1.0f, 3);
    double diff = 0.0, ref = 0.0;
    for (int blk = 0; blk < 30; ++blk)
    {
        ba.clear(); bb.clear(); m.clear();
        juce::MidiBuffer m2;
        a.processBlock (ba, m); b.processBlock (bb, m2);
        for (int i = 0; i < 256; ++i)
        {
            const float x = ba.getSample (0, i), y = bb.getSample (0, i);
            diff += std::abs (x - y); ref += std::abs (x);
        }
    }
    INFO ("flat-EQ-on vs EQ-off L1 diff=" << diff << " ref=" << ref);
    REQUIRE (diff < ref * 0.02 + 1e-6);   // a flat 5-band EQ is ~transparent (post-crossfade)
}

TEST_CASE ("per-part EQ persists across focus round-trips (kit <-> synth)", "[plugin][eq][kit][editfocus]")
{
    VASynthProcessor p; p.prepareToPlay (48000.0, 256);
    p.setPartKit (3, VASynthProcessor::factoryKit ("808 Basics"));   // P4 = kit

    // Focus the kit, dial in a distinctive EQ.
    p.setEditFocus (3);
    set01 (p, ParamID::peqOn, 1.0f);
    setV  (p, ParamID::peqB1Gain, 7.5f);
    setV  (p, ParamID::peqB5Gain, -4.0f);

    // Focus a synth part, set a DIFFERENT EQ there.
    p.setEditFocus (0);
    set01 (p, ParamID::peqOn, 1.0f);
    setV  (p, ParamID::peqB3Gain, 3.0f);
    const float synthB3 = p.apvts.getRawParameterValue (ParamID::peqB3Gain)->load();

    // Back to the kit: its EQ must be exactly what we left.
    p.setEditFocus (3);
    REQUIRE (p.apvts.getRawParameterValue (ParamID::peqB1Gain)->load() == Catch::Approx (7.5f).margin (0.05));
    REQUIRE (p.apvts.getRawParameterValue (ParamID::peqB5Gain)->load() == Catch::Approx (-4.0f).margin (0.05));

    // And the synth part kept its own.
    p.setEditFocus (0);
    REQUIRE (p.apvts.getRawParameterValue (ParamID::peqB3Gain)->load() == Catch::Approx (synthB3).margin (0.01));
}

TEST_CASE ("focusing a kit dims the synth panels; a synth part does not", "[plugin][eq][kit][screenshot]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VASynthProcessor p; p.prepareToPlay (48000.0, 256);
    p.setPartKit (3, VASynthProcessor::factoryKit ("808 Basics"));

    // Synth part focused -> no scrim.
    p.setEditFocus (0);
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    ed->setSize (1760, 1000);
    bool scrimVisibleSynth = false, scrimVisibleKit = false;
    for (auto* c : ed->getChildren()) if (dynamic_cast<KitScrim*> (c)) scrimVisibleSynth = c->isVisible();
    REQUIRE_FALSE (scrimVisibleSynth);

    // Focus the kit -> the scrim shows (resized() refreshes it).
    p.setEditFocus (3);
    ed->resized();
    KitScrim* scrim = nullptr;
    for (auto* c : ed->getChildren()) if (auto* ks = dynamic_cast<KitScrim*> (c)) scrim = ks;
    REQUIRE (scrim != nullptr);
    scrimVisibleKit = scrim->isVisible();
    REQUIRE (scrimVisibleKit);

    auto img = ed->createComponentSnapshot (ed->getLocalBounds(), false, 1.0f);
    REQUIRE (img.isValid());
    juce::File out (juce::String (VASYNTH_DOCS_DIR) + "/smoke/kit-focus-scrim.png");
    out.getParentDirectory().createDirectory(); out.deleteFile();
    juce::FileOutputStream os (out); REQUIRE (os.openedOk());
    juce::PNGImageFormat png; REQUIRE (png.writeImageToStream (img, os));
}
