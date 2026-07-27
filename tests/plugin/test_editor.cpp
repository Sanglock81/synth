// ============================================================================
// Custom editor: open/close, layout, state round-trip with the editor live
// (attachments), MIDI-learn badge query, and a committed layout screenshot.
// (Touch/multitouch, fullscreen, arm's-length readability are hand-verified.)
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "UI/TopBar.h"
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

    // Collect the parameter IDs of every APVTS-bound (learnable) control in the tree.
    void collectParamIds (juce::Component& c, juce::StringArray& out)
    {
        for (auto* ch : c.getChildren())
        {
            if (auto* lc = dynamic_cast<LearnableComponent*> (ch)) out.addIfNotAlreadyThere (lc->parameterID());
            collectParamIds (*ch, out);
        }
    }
}

TEST_CASE ("preset menu: factory categories are submenus; user presets carry Load/Rename/Delete", "[plugin][preset][menu]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    PresetManager pm (p.apvts);
    const auto& lib = p.factoryPresetLibrary();

    const auto uname = "ut-menu-" + juce::String (juce::Random::getSystemRandom().nextInt (1'000'000));
    REQUIRE (pm.save (uname, "Lead"));

    juce::String loaded, renamed, deleted;
    juce::PopupMenu m;
    TopBar::buildPresetMenu (m, lib, pm,
        [&] (const juce::String& n) { loaded  = n; },
        [&] (const juce::String& n) { renamed = n; },
        [&] (const juce::String& n) { deleted = n; });

    // Copy out a named submenu (safe past the iterator's lifetime).
    auto sub = [] (juce::PopupMenu& menu, const juce::String& text, juce::PopupMenu& out) -> bool
    {
        for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
            if (it.getItem().text == text && it.getItem().subMenu != nullptr) { out = *it.getItem().subMenu; return true; }
        return false;
    };
    // Fire a named item's action.
    auto fire = [] (juce::PopupMenu& menu, const juce::String& text) -> bool
    {
        for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
            if (it.getItem().text == text && it.getItem().action) { it.getItem().action(); return true; }
        return false;
    };

    // Init loads directly from the top level.
    REQUIRE (fire (m, "Init"));
    REQUIRE (loaded == "Init");

    // Each factory category is a collapsible submenu; its patches load on click (real wiring).
    juce::PopupMenu bass;
    REQUIRE (sub (m, "Bass", bass));
    const auto aBass = lib.namesInCategory ("Bass")[0];
    REQUIRE (fire (bass, aBass));
    REQUIRE (loaded == aBass);

    // The user patch lives under "My Presets" as its own Load/Rename/Delete submenu.
    juce::PopupMenu mine, actions;
    REQUIRE (sub (m, "My Presets", mine));
    REQUIRE (sub (mine, uname, actions));
    REQUIRE (fire (actions, "Load"));    REQUIRE (loaded  == uname);
    REQUIRE (fire (actions, "Rename\xe2\x80\xa6")); REQUIRE (renamed == uname);   // "Rename…" (UTF-8 ellipsis)
    REQUIRE (fire (actions, "Delete"));  REQUIRE (deleted == uname);

    pm.presetDir().getChildFile (uname + ".vasynth").deleteFile();
}

TEST_CASE ("editor surfaces the voice controls + key params (drop regression)", "[plugin][editor][regression]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    ed->setSize (1760, 1000);

    juce::StringArray ids;
    collectParamIds (*ed, ids);

    // poly/mono/legato + glide were dropped in the R2 rebuild — guard against re-dropping
    // these (and a few other must-haves) when the layout changes again.
    for (auto* id : { "poly_mode", "glide_time", "master_gain", "filter_cutoff",
                      "arp_mode", "loop_bars", "macro1" })
        { INFO ("missing control for " << id); REQUIRE (ids.contains (id)); }
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
