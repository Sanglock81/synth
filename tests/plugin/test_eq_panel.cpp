// ============================================================================
// K1 — the consolidated per-part EQ section (EQPanel), wired to a real processor.
//   * screenshot sign-off render of the actual section (docs/smoke/eq-section-wired.png)
//   * slider-gesture smoke: vertical drag = gain, horizontal drag = freq, dot = band on/off
//   * editing any band auto-enables the section (no silent-EQ trap)
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginProcessor.h"
#include "UI/EQPanel.h"
#include <memory>

#ifndef VASYNTH_DOCS_DIR
 #define VASYNTH_DOCS_DIR "."
#endif

namespace
{
    juce::MouseEvent evt (juce::Component& c, juce::Point<float> pos, juce::Point<float> downPos, bool dragged)
    {
        auto src  = juce::Desktop::getInstance().getMainMouseSource();
        auto mods = juce::ModifierKeys().withFlags (juce::ModifierKeys::leftButtonModifier);
        const auto t = juce::Time::getCurrentTime();
        return juce::MouseEvent (src, pos, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, t, downPos, t, 1, dragged);
    }
    juce::Point<float> fp (juce::Point<int> p) { return { (float) p.x, (float) p.y }; }

    // Force a paint so EQPanel's geometry (barR / bodyR) is populated before hit-testing.
    void primeGeometry (EQPanel& panel) { panel.createComponentSnapshot (panel.getLocalBounds(), false, 1.0f); }
}

TEST_CASE ("EQPanel wired screenshot render for sign-off", "[plugin][eq][screenshot]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VASynthProcessor p;

    // A representative, musical-looking state: section on, a smile curve, one band off.
    p.apvts.getParameter (ParamID::peqOn)->setValueNotifyingHost (1.0f);
    auto setDb = [&] (const char* id, float db) {
        auto* pr = p.apvts.getParameter (id); auto& r = pr->getNormalisableRange();
        pr->setValueNotifyingHost (r.convertTo0to1 (db));
    };
    setDb (ParamID::peqB1Gain,  4.0f);
    setDb (ParamID::peqB2Gain, -3.0f);
    setDb (ParamID::peqB3Gain,  2.0f);
    setDb (ParamID::peqB4Gain,  5.0f);
    p.apvts.getParameter (ParamID::peqB3On)->setValueNotifyingHost (0.0f);   // band 3 off

    EQPanel panel (p);
    panel.setSize (286, 210);
    auto img = panel.createComponentSnapshot (panel.getLocalBounds(), false, 2.0f);
    REQUIRE (img.isValid());

    juce::File out (juce::String (VASYNTH_DOCS_DIR) + "/smoke/eq-section-wired.png");
    out.getParentDirectory().createDirectory(); out.deleteFile();
    juce::FileOutputStream os (out); REQUIRE (os.openedOk());
    juce::PNGImageFormat png; REQUIRE (png.writeImageToStream (img, os));
}

TEST_CASE ("EQPanel: vertical drag moves GAIN, leaves freq", "[plugin][eq][gesture]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VASynthProcessor p;
    EQPanel panel (p); panel.setSize (286, 210); primeGeometry (panel);

    const float freq0 = p.apvts.getRawParameterValue (ParamID::peqB1Freq)->load();
    const float gain0 = p.apvts.getRawParameterValue (ParamID::peqB1Gain)->load();

    auto c = panel.testBandCentre (0);
    const auto down = fp (c);
    panel.mouseDown (evt (panel, down, down, false));
    panel.mouseDrag (evt (panel, { down.x, down.y - 40.0f }, down, true));   // drag UP
    panel.mouseUp   (evt (panel, { down.x, down.y - 40.0f }, down, true));

    REQUIRE (p.apvts.getRawParameterValue (ParamID::peqB1Gain)->load() > gain0 + 0.5f);   // gain rose
    REQUIRE (p.apvts.getRawParameterValue (ParamID::peqB1Freq)->load() == Catch::Approx (freq0)); // freq untouched
}

TEST_CASE ("EQPanel: horizontal drag moves FREQ, leaves gain", "[plugin][eq][gesture]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VASynthProcessor p;
    EQPanel panel (p); panel.setSize (286, 210); primeGeometry (panel);

    const float freq0 = p.apvts.getRawParameterValue (ParamID::peqB2Freq)->load();
    const float gain0 = p.apvts.getRawParameterValue (ParamID::peqB2Gain)->load();

    auto c = panel.testBandCentre (1);
    const auto down = fp (c);
    panel.mouseDown (evt (panel, down, down, false));
    panel.mouseDrag (evt (panel, { down.x + 60.0f, down.y }, down, true));   // drag RIGHT
    panel.mouseUp   (evt (panel, { down.x + 60.0f, down.y }, down, true));

    REQUIRE (p.apvts.getRawParameterValue (ParamID::peqB2Freq)->load() > freq0 + 1.0f);   // freq rose
    REQUIRE (p.apvts.getRawParameterValue (ParamID::peqB2Gain)->load() == Catch::Approx (gain0)); // gain untouched
}

TEST_CASE ("EQPanel: editing a band auto-enables the section (no silent EQ)", "[plugin][eq][gesture]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VASynthProcessor p;
    p.apvts.getParameter (ParamID::peqOn)->setValueNotifyingHost (0.0f);    // start OFF
    EQPanel panel (p); panel.setSize (286, 210); primeGeometry (panel);

    auto c = panel.testBandCentre (0);
    const auto down = fp (c);
    panel.mouseDown (evt (panel, down, down, false));
    panel.mouseDrag (evt (panel, { down.x, down.y - 30.0f }, down, true));
    panel.mouseUp   (evt (panel, { down.x, down.y - 30.0f }, down, true));

    REQUIRE (p.apvts.getRawParameterValue (ParamID::peqOn)->load() > 0.5f);  // now ON
}

TEST_CASE ("EQPanel: tapping the dot toggles that band on/off", "[plugin][eq][gesture]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VASynthProcessor p;
    EQPanel panel (p); panel.setSize (286, 210); primeGeometry (panel);

    const bool before = p.apvts.getRawParameterValue (ParamID::peqB4On)->load() > 0.5f;
    const auto dot = fp (panel.testBandDot (3));
    panel.mouseDown (evt (panel, dot, dot, false));
    panel.mouseUp   (evt (panel, dot, dot, false));
    const bool after = p.apvts.getRawParameterValue (ParamID::peqB4On)->load() > 0.5f;
    REQUIRE (after != before);
}
