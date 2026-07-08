#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "VASynthLookAndFeel.h"
#include "../PluginProcessor.h"

// ============================================================================
// INPUTS routing dialog (7C). A modal component (no focus leaks — same pattern as
// the Save dialog) listing every playing surface: QWERTY first, then each MIDI
// input by name. Per surface: a routing choice (Live / Part 1-3), a preset picker
// for the assigned locked part (assigning bakes it), and a live-activity dot that
// blinks on incoming events (a dead cable is diagnosable at a glance).
// ============================================================================

class InputsDialog : public juce::Component,
                     private juce::Timer
{
public:
    explicit InputsDialog (VASynthProcessor& p) : proc (p)
    {
        setWantsKeyboardFocus (false);
        buildSurfaceList();
        for (auto& r : rows) addRow (r);
        setSize (560, 70 + (int) rows.size() * rowH);
        startTimerHz (12);   // poll activity
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (VASynthLookAndFeel::panel());
        g.setColour (VASynthLookAndFeel::ink());
        g.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
        g.drawText ("INPUTS  -  route each surface to a part", getLocalBounds().removeFromTop (34).reduced (14, 0),
                    juce::Justification::centredLeft, false);

        // activity dots
        for (auto& r : rows)
        {
            const bool active = r.lastSeen != proc.surfaceActivity (r.name) || r.blink > 0;
            g.setColour (active ? VASynthLookAndFeel::accent() : VASynthLookAndFeel::track());
            g.fillEllipse ((float) r.dotX, (float) r.dotY, 10.0f, 10.0f);
        }
    }

    void resized() override
    {
        int y = 40;
        for (auto& r : rows)
        {
            r.dotX = 16; r.dotY = y + rowH / 2 - 5;
            r.route->setBounds (200, y + 4, 110, rowH - 8);
            r.preset->setBounds (320, y + 4, 220, rowH - 8);
            y += rowH;
        }
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        g.setColour (VASynthLookAndFeel::ink());
        g.setFont (juce::Font (juce::FontOptions (13.0f)));
        int y = 40;
        for (auto& r : rows) { g.drawText (r.name, 34, y, 160, rowH, juce::Justification::centredLeft, true); y += rowH; }
    }

    // Show modally, centred on the editor. Reuses the Save-dialog modal discipline so
    // no focus leaks to the main panel (QWERTY note input resumes on close).
    static void show (VASynthProcessor& proc, juce::Component* parent, std::function<void()> onClose)
    {
        auto dlg = std::make_unique<InputsDialog> (proc);
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned (dlg.release());
        o.dialogTitle = "Inputs";
        o.dialogBackgroundColour = VASynthLookAndFeel::panel();
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = false;
        if (parent != nullptr) o.componentToCentreAround = parent;
        auto* w = o.launchAsync();
        if (w != nullptr)
            w->enterModalState (true, juce::ModalCallbackFunction::create ([onClose] (int) { if (onClose) onClose(); }), false);
    }

private:
    static constexpr int rowH = 34;

    struct Row
    {
        juce::String name;
        std::unique_ptr<juce::ComboBox> route, preset;
        std::uint32_t lastSeen = 0;
        int blink = 0, dotX = 0, dotY = 0;
    };

    void buildSurfaceList()
    {
        rows.clear();
        rows.push_back ({ "QWERTY", {}, {}, 0, 0, 0, 0 });
        for (auto& d : juce::MidiInput::getAvailableDevices())
            rows.push_back ({ d.name, {}, {}, 0, 0, 0, 0 });
    }

    void addRow (Row& r)
    {
        r.route = std::make_unique<juce::ComboBox>();
        r.route->setWantsKeyboardFocus (false);
        r.route->addItem ("Live",   1);
        r.route->addItem ("Part 1", 2);
        r.route->addItem ("Part 2", 3);
        r.route->addItem ("Part 3", 4);
        r.route->setSelectedId (proc.getSurfaceRouting (r.name) + 1, juce::dontSendNotification);
        addAndMakeVisible (*r.route);

        r.preset = std::make_unique<juce::ComboBox>();
        r.preset->setWantsKeyboardFocus (false);
        r.preset->setTextWhenNothingSelected ("(part preset)");
        r.preset->addItem ("Init", 1);
        int id = 2;
        for (auto& fp : proc.factoryPresetLibrary().all()) r.preset->addItem (fp.name, id++);
        addAndMakeVisible (*r.preset);

        auto* route = r.route.get();
        auto* preset = r.preset.get();
        const juce::String name = r.name;
        auto apply = [this, route, preset, name] { applyRouting (name, route->getSelectedId() - 1, preset->getText()); };
        route->onChange = apply;
        preset->onChange = apply;
        updatePresetEnabled (r);
    }

public:
    // The action a surface row performs (route -> part, and bake the preset for a
    // locked part). Public so the full dialog path is integration-testable without
    // driving the GUI, and shared by both combos' onChange.
    void applyRouting (const juce::String& surface, int part, const juce::String& preset)
    {
        proc.setSurfaceRouting (surface, part);
        if (part >= 1 && preset.isNotEmpty() && preset != "(part preset)")
            proc.setPartPreset (part, preset);
    }

    // Surface rows the dialog presents (QWERTY first, then each MIDI input).
    int          numRows() const { return (int) rows.size(); }
    juce::String rowName (int i) const { return (i >= 0 && i < (int) rows.size()) ? rows[(std::size_t) i].name : juce::String(); }

private:

    void updatePresetEnabled (Row& r)
    {
        const int part = r.route->getSelectedId() - 1;
        r.preset->setEnabled (part >= 1);
        if (part >= 1) r.preset->setText (proc.getPartPreset (part), juce::dontSendNotification);
    }

    void timerCallback() override
    {
        for (auto& r : rows)
        {
            const auto now = proc.surfaceActivity (r.name);
            if (now != r.lastSeen) { r.lastSeen = now; r.blink = 3; }
            else if (r.blink > 0)  --r.blink;
            updatePresetEnabled (r);
        }
        repaint();
    }

    VASynthProcessor& proc;
    std::vector<Row> rows;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InputsDialog)
};
