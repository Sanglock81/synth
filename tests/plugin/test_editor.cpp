// ============================================================================
// Custom editor: open/close, layout, state round-trip with the editor live
// (attachments), MIDI-learn badge query, and a committed layout screenshot.
// (Touch/multitouch, fullscreen, arm's-length readability are hand-verified.)
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <memory>

#ifndef VASYNTH_DOCS_DIR
 #define VASYNTH_DOCS_DIR "."
#endif

namespace
{
    bool anyDescendantWantsFocus (juce::Component& c)
    {
        for (auto* ch : c.getChildren())
        {
            if (ch->getWantsKeyboardFocus()) return true;
            if (anyDescendantWantsFocus (*ch)) return true;
        }
        return false;
    }
}

TEST_CASE ("no editor descendant wants keyboard focus (QWERTY never starved)", "[plugin][editor][focus]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    ed->setSize (2760, 660);
    // Only the editor root is the QWERTY receiver; no child (faders, buttons,
    // combos, preset panel) may claim keyboard focus. A persistent text field
    // here would type keys instead of playing notes at startup.
    REQUIRE_FALSE (anyDescendantWantsFocus (*ed));
}

TEST_CASE ("editor opens, lays out, and closes without crashing", "[plugin][editor]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    REQUIRE (ed != nullptr);
    ed->setSize (2760, 660);
    REQUIRE (ed->getWidth() == 2760);
    // open/close storm (also exercised by pluginval; leak-checked under ASan)
    ed.reset();
    for (int i = 0; i < 5; ++i) { std::unique_ptr<juce::AudioProcessorEditor> e (p.createEditor()); e->setSize (1000, 560); }
    SUCCEED();
}

TEST_CASE ("state round-trips with the editor open (attachments stay in sync)", "[plugin][editor][state]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor src;
    std::unique_ptr<juce::AudioProcessorEditor> ed (src.createEditor());
    ed->setSize (2760, 660);

    // Set some parameters (as a fader/segmented would, via the attachment path).
    src.apvts.getParameter ("filter_cutoff")->setValueNotifyingHost (0.33f);
    src.apvts.getParameter ("osc1_wave")->setValueNotifyingHost (1.0f);      // last choice
    src.apvts.getParameter ("amp_release")->setValueNotifyingHost (0.7f);

    juce::MemoryBlock blob;
    src.getStateInformation (blob);

    VASynthProcessor dst;
    std::unique_ptr<juce::AudioProcessorEditor> ed2 (dst.createEditor());     // editor open during restore
    ed2->setSize (2760, 660);
    dst.setStateInformation (blob.getData(), (int) blob.getSize());

    REQUIRE (dst.apvts.getParameter ("filter_cutoff")->getValue() == Catch::Approx (0.33f).margin (1e-4));
    REQUIRE (dst.apvts.getParameter ("osc1_wave")->getValue()     == Catch::Approx (1.0f).margin (1e-4));
    REQUIRE (dst.apvts.getParameter ("amp_release")->getValue()   == Catch::Approx (0.7f).margin (1e-4));
}

TEST_CASE ("MIDI-learn badge query reflects a learned CC", "[plugin][editor][midilearn]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 64);
    auto& learn = p.getMidiLearn();

    // Default Launchkey map -> CC21 is macro1 (badge should show it).
    REQUIRE (learn.getCCForParam ("macro1") == 21);

    // Arm + bind a new CC, then the badge query reflects it.
    learn.armLearn ("osc_mix");
    REQUIRE (learn.isLearningParam ("osc_mix"));
    juce::AudioBuffer<float> buf (2, 64); buf.clear();
    juce::MidiBuffer midi; midi.addEvent (juce::MidiMessage::controllerEvent (1, 55, 100), 0);
    p.processBlock (buf, midi);
    REQUIRE (learn.getCCForParam ("osc_mix") == 55);
    REQUIRE_FALSE (learn.isLearningParam ("osc_mix"));

    learn.clearParam ("osc_mix");
    REQUIRE (learn.getCCForParam ("osc_mix") == -1);
}

TEST_CASE ("render a layout screenshot to docs/editor.png", "[plugin][editor][screenshot]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    ed->setSize (1760, 1000);

    auto img = ed->createComponentSnapshot (ed->getLocalBounds(), false, 1.0f);
    REQUIRE (img.isValid());
    REQUIRE (img.getWidth() == 1760);

    juce::File out (juce::String (VASYNTH_DOCS_DIR) + "/editor.png");
    out.getParentDirectory().createDirectory();
    out.deleteFile();
    juce::FileOutputStream os (out);
    REQUIRE (os.openedOk());
    juce::PNGImageFormat png;
    REQUIRE (png.writeImageToStream (img, os));
}
