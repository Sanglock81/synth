// ============================================================================
// Looper is decoupled from edit focus (task #47). Each lane N == part N has its own
// transport and captures part N regardless of the edit/play focus. Recording on lane 2 and
// switching focus among all parts throughout must leave lane 2 (and only lane 2) recorded,
// byte-identical to a run that never switched focus.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "PluginProcessor.h"

namespace
{
    void s01 (VASynthProcessor& p, const char* id, float v) { p.apvts.getParameter (id)->setValueNotifyingHost (v); }
    void setVal (VASynthProcessor& p, const char* id, float v)
    { p.apvts.getParameter (id)->setValueNotifyingHost (p.apvts.getParameter (id)->convertTo0to1 (v)); }
}

TEST_CASE ("looper: recording a lane is independent of edit focus", "[plugin][looper][isolation]")
{
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    p.setPartPreset (2, "Fat Saw Bass");
    setVal (p, ParamID::tempo, 200.0f);
    s01 (p, ParamID::loopRec3, 1.0f);                // lane 2 (P3) REC + PLAY
    s01 (p, ParamID::loopPlay3, 1.0f);

    juce::AudioBuffer<float> buf (2, 128); juce::MidiBuffer m;
    auto block = [&] { buf.clear(); m.clear(); p.processBlock (buf, m); };
    block(); block();                                // engage REC at the boundary

    const int notes[] { 48, 52, 55, 59 };
    for (int i = 0; i < 24; ++i)
    {
        p.setEditFocus (i % 4);                      // switch focus among ALL parts throughout
        if (i < 4) p.routeNoteOn (notes[i], 0.9f, 2);
        block();
    }

    // Only lane 2 recorded — focus churn never leaked the recording to another lane.
    REQUIRE (p.loopLaneHasContent (2));
    REQUIRE_FALSE (p.loopLaneHasContent (0));
    REQUIRE_FALSE (p.loopLaneHasContent (1));
    REQUIRE_FALSE (p.loopLaneHasContent (3));
}

TEST_CASE ("looper: lane 2 records the same whether or not focus is switched", "[plugin][looper][isolation]")
{
    auto count = [] (bool switchFocus)
    {
        VASynthProcessor p; p.prepareToPlay (48000.0, 128);
        p.setPartPreset (2, "Fat Saw Bass");
        setVal (p, ParamID::tempo, 200.0f);
        s01 (p, ParamID::loopRec3, 1.0f);
        s01 (p, ParamID::loopPlay3, 1.0f);
        juce::AudioBuffer<float> buf (2, 128); juce::MidiBuffer m;
        auto block = [&] { buf.clear(); m.clear(); p.processBlock (buf, m); };
        block(); block();
        const int notes[] { 48, 52, 55, 59 };
        for (int i = 0; i < 24; ++i)
        {
            if (switchFocus) p.setEditFocus (i % 4);
            if (i < 4) p.routeNoteOn (notes[i], 0.9f, 2);
            block();
        }
        return p.loopLaneEventCount (2);
    };

    const int noSwitch = count (false);
    const int withSwitch = count (true);
    REQUIRE (noSwitch > 0);
    REQUIRE (withSwitch == noSwitch);   // focus churn changed NOTHING about lane 2's recording
}
