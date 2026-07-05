#include "PluginEditor.h"

// Preset controls (Random / Save / name / Load) inside the Global section.
// A small owned panel so the Section's even FlexBox layout treats it as one cell.
void VASynthEditor::buildGlobalExtras (Section& s)
{
    struct PresetPanel : public juce::Component
    {
        PresetPanel (VASynthProcessor& p, PresetManager& pm) : proc (p), presets (pm)
        {
            random.setButtonText ("Random");
            random.setWantsKeyboardFocus (false);
            random.onClick = [this]
            {
                juce::Random rng;
                presets.randomize (rng);
            };
            addAndMakeVisible (random);

            name.setTextToShowWhenEmpty ("preset name", VASynthLookAndFeel::dim());
            name.setWantsKeyboardFocus (true);      // typing here suppresses QWERTY (editor checks focus)
            addAndMakeVisible (name);

            save.setButtonText ("Save");
            save.setWantsKeyboardFocus (false);
            save.onClick = [this]
            {
                if (presets.save (name.getText())) refreshList();
            };
            addAndMakeVisible (save);

            load.setTextWhenNothingSelected ("Load preset...");
            load.setWantsKeyboardFocus (false);
            load.onChange = [this]
            {
                const auto n = load.getText();
                if (n.isNotEmpty()) presets.load (n);
            };
            addAndMakeVisible (load);
            refreshList();
        }

        void refreshList()
        {
            load.clear (juce::dontSendNotification);
            int id = 1;
            for (auto& n : presets.getPresetNames()) load.addItem (n, id++);
        }

        void resized() override
        {
            auto r = getLocalBounds();
            const int h = juce::jmax (24, r.getHeight() / 4 - 4);
            random.setBounds (r.removeFromTop (h)); r.removeFromTop (4);
            name.setBounds   (r.removeFromTop (h)); r.removeFromTop (4);
            save.setBounds   (r.removeFromTop (h)); r.removeFromTop (4);
            load.setBounds   (r.removeFromTop (h));
        }

        VASynthProcessor& proc;
        PresetManager& presets;
        juce::TextButton random, save;
        juce::TextEditor name;
        juce::ComboBox load;
    };

    auto panel = std::make_unique<PresetPanel> (proc, presets);
    s.addAndMakeVisible (*panel);
    presetPanel = std::move (panel);
}
