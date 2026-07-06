#include "PluginEditor.h"

// Preset controls (Random / Save / Load) inside the Global section.
//
// No persistent text field lives on the main panel — that would take keyboard
// focus at startup and starve the QWERTY note path. Naming happens in a modal
// Save dialog; focus returns to the editor when it closes.
void VASynthEditor::buildGlobalExtras (Section& s)
{
    struct PresetPanel : public juce::Component
    {
        PresetPanel (VASynthProcessor& p, PresetManager& pm, std::function<void()> restore)
            : proc (p), presets (pm), restoreFocus (std::move (restore))
        {
            // Give the panel enough width that button labels never truncate.
            getProperties().set ("layoutFlex", 3.4);

            for (auto* b : { &random, &save })
            {
                b->setWantsKeyboardFocus (false);
                addAndMakeVisible (b);
            }
            random.setButtonText ("Random");
            random.onClick = [this] { juce::Random rng; presets.randomize (rng); };

            save.setButtonText ("Save");
            save.onClick = [this] { showSaveDialog(); };

            load.setTextWhenNothingSelected ("Load");
            load.setWantsKeyboardFocus (false);
            load.onChange = [this]
            {
                const auto n = load.getText();
                if (n.isEmpty()) return;
                if (n == "Init")                                          proc.loadInitPreset();
                else if (proc.factoryPresetLibrary().byName (n) != nullptr) proc.loadFactoryPreset (n);
                else                                                      presets.load (n);
                load.setSelectedId (0, juce::dontSendNotification);       // back to "Load" so re-selecting works
            };
            addAndMakeVisible (load);
            refreshList();
        }

        void showSaveDialog()
        {
            auto* aw = new juce::AlertWindow ("Save Preset", "Preset name:",
                                              juce::MessageBoxIconType::NoIcon, this);
            aw->addTextEditor ("name", "");
            aw->addButton ("Save",   1, juce::KeyPress (juce::KeyPress::returnKey));
            aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
            aw->enterModalState (true, juce::ModalCallbackFunction::create (
                [this, aw] (int result)
                {
                    if (result == 1 && presets.save (aw->getTextEditorContents ("name")))
                        refreshList();
                    if (restoreFocus) restoreFocus();      // QWERTY resumes
                }), true);                                  // deleteWhenDismissed
        }

        void refreshList()
        {
            load.clear (juce::dontSendNotification);
            int id = 1;
            load.addItem ("Init", id++);

            // Factory presets grouped under category headings.
            const auto& lib = proc.factoryPresetLibrary();
            for (auto& cat : lib.categories())
            {
                load.addSectionHeading (cat);
                for (auto& fp : lib.all())
                    if (fp.category == cat) load.addItem (fp.name, id++);
            }

            // User presets last, under their own heading.
            auto userNames = presets.getPresetNames();
            if (! userNames.isEmpty())
            {
                load.addSectionHeading ("User");
                for (auto& n : userNames) load.addItem (n, id++);
            }
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (0, 2);
            const int gap = 6;
            const int h = juce::jmax (30, (r.getHeight() - 2 * gap) / 3);
            random.setBounds (r.removeFromTop (h)); r.removeFromTop (gap);
            save.setBounds   (r.removeFromTop (h)); r.removeFromTop (gap);
            load.setBounds   (r.removeFromTop (h));
        }

        VASynthProcessor& proc;
        PresetManager& presets;
        std::function<void()> restoreFocus;
        juce::TextButton random, save;
        juce::ComboBox load;
    };

    auto panel = std::make_unique<PresetPanel> (proc, presets, [this] { restoreQwertyFocus(); });
    s.addAndMakeVisible (*panel);
    presetPanel = std::move (panel);
}
