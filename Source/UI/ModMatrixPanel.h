#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "VASynthLookAndFeel.h"
#include "../PluginProcessor.h"

// ============================================================================
// MOD MATRIX overlay (#56). A modal panel (same no-focus-leak discipline as the Save /
// INPUTS dialogs) listing the FOCUSED part's 8 routing slots. Each row: a source, an
// arrow, a destination, a bipolar depth slider (gold = positive, cyan = inverted), and a
// remove. Routes are usually made by the LINK gesture (arm a source, tap a knob); this
// overlay is where you inspect, re-point, re-depth, invert and delete them — and you can
// build a route here from scratch with the two dropdowns.
// ============================================================================

class ModMatrixPanel : public juce::Component,
                       private juce::Timer
{
public:
    static juce::StringArray sourceNames()
    {
        return { "\xe2\x80\x94", "LFO 1", "LFO 2", "LFO 3", "Mod Env", "Amp Env", "Velocity", "Note",
                 "Mod Wheel", "Pitch Bend", "Random",
                 "Macro 1", "Macro 2", "Macro 3", "Macro 4", "Macro 5", "Macro 6", "Macro 7", "Macro 8" };
    }
    static juce::StringArray destNames()
    {
        return { "\xe2\x80\x94", "Pitch", "Cutoff", "Resonance", "Pulse Width", "Amp",
                 "Wave Pos", "Osc 1 Lvl", "Osc 2 Lvl", "Osc 3 Lvl" };
    }

    explicit ModMatrixPanel (VASynthProcessor& p) : proc (p)
    {
        setWantsKeyboardFocus (false);
        for (int i = 0; i < VASynthProcessor::kModSlots; ++i)
        {
            auto& r = rows[(std::size_t) i];
            r.src  = std::make_unique<juce::ComboBox>();
            r.dst  = std::make_unique<juce::ComboBox>();
            r.depth = std::make_unique<juce::Slider> (juce::Slider::LinearHorizontal, juce::Slider::NoTextBox);
            fill (*r.src, sourceNames());
            fill (*r.dst, destNames());
            r.depth->setRange (-100.0, 100.0, 1.0);
            r.depth->setDoubleClickReturnValue (true, 0.0);
            for (auto* c : { (juce::Component*) r.src.get(), (juce::Component*) r.dst.get(), (juce::Component*) r.depth.get() })
                { c->setWantsKeyboardFocus (false); addAndMakeVisible (c); }
            r.remove.setButtonText ("\xe2\x9c\x95");
            r.remove.setWantsKeyboardFocus (false);
            addAndMakeVisible (r.remove);

            const int idx = i;
            r.src->onChange   = [this, idx] { const int s = rows[(std::size_t) idx].src->getSelectedId() - 1;
                                              const auto cur = proc.getModSlot (-1, idx);
                                              proc.setModSlot (-1, idx, s, cur.dest, cur.depth); refresh(); };
            r.dst->onChange   = [this, idx] { const int d = rows[(std::size_t) idx].dst->getSelectedId() - 1;
                                              const auto cur = proc.getModSlot (-1, idx);
                                              proc.setModSlot (-1, idx, cur.source, d, cur.depth); refresh(); };
            r.depth->onValueChange = [this, idx] { proc.setModDepth (-1, idx, (float) rows[(std::size_t) idx].depth->getValue() / 100.0f); };
            r.remove.onClick  = [this, idx] { proc.clearModSlot (-1, idx); refresh(); };
        }
        refresh();
        setSize (560, 96 + VASynthProcessor::kModSlots * 40);
        startTimerHz (20);   // live modulation bars
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (VASynthLookAndFeel::panel());
        g.setColour (VASynthLookAndFeel::ink());
        g.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
        g.drawText ("MOD MATRIX", getLocalBounds().removeFromTop (34).reduced (16, 0), juce::Justification::centredLeft, false);
        g.setColour (VASynthLookAndFeel::accent());
        g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::bold)));
        g.drawText ("PART " + juce::String (proc.editFocus() + 1),
                    getLocalBounds().removeFromTop (34).reduced (16, 0), juce::Justification::centredRight, false);
        g.setColour (VASynthLookAndFeel::dim());
        g.setFont (juce::Font (juce::FontOptions (11.5f)));
        g.drawText ("Arm LINK and tap a knob to make a route \xe2\x80\x94 or pick a source + destination here.",
                    juce::Rectangle<int> (16, getHeight() - 24, getWidth() - 32, 16), juce::Justification::centredLeft, false);
    }

    void resized() override
    {
        auto c = getLocalBounds().reduced (14, 0);
        c.removeFromTop (40);
        for (int i = 0; i < VASynthProcessor::kModSlots; ++i)
        {
            auto row = c.removeFromTop (40).reduced (0, 4);
            auto& r = rows[(std::size_t) i];
            r.src->setBounds  (row.removeFromLeft (120));               row.removeFromLeft (22);   // arrow gap
            r.dst->setBounds  (row.removeFromLeft (120));               row.removeFromLeft (10);
            r.remove.setBounds (row.removeFromRight (34).reduced (2, 1)); row.removeFromRight (8);
            r.depthArea = row;
            r.depth->setBounds (row);
        }
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        // arrows between the two dropdowns + the live-modulation tick on each depth bar
        g.setColour (VASynthLookAndFeel::accent());
        g.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
        for (int i = 0; i < VASynthProcessor::kModSlots; ++i)
        {
            auto& r = rows[(std::size_t) i];
            const auto sb = r.src->getBounds();
            g.drawText ("\xe2\x86\x92", juce::Rectangle<int> (sb.getRight(), sb.getY(), 22, sb.getHeight()),
                        juce::Justification::centred, false);
        }
    }

    static void show (VASynthProcessor& proc, juce::Component* parent, std::function<void()> onClose)
    {
        auto dlg = std::make_unique<ModMatrixPanel> (proc);
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned (dlg.release());
        o.dialogTitle = "Mod Matrix";
        o.dialogBackgroundColour = VASynthLookAndFeel::panel();
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = false;
        if (parent != nullptr) o.componentToCentreAround = parent;
        if (auto* w = o.launchAsync())
            w->enterModalState (true, juce::ModalCallbackFunction::create ([onClose] (int) { if (onClose) onClose(); }), false);
    }

    // ---- test hooks: read the controls back, and drive an edit through the real handlers --
    int   rowSource (int i) const { return rows[(std::size_t) i].src->getSelectedId() - 1; }
    int   rowDest   (int i) const { return rows[(std::size_t) i].dst->getSelectedId() - 1; }
    float rowDepth  (int i) const { return (float) rows[(std::size_t) i].depth->getValue() / 100.0f; }
    void  pickForTest (int i, int source, int dest, float depth)
    {
        rows[(std::size_t) i].src->setSelectedId (source + 1, juce::sendNotificationSync);
        rows[(std::size_t) i].dst->setSelectedId (dest + 1,   juce::sendNotificationSync);
        rows[(std::size_t) i].depth->setValue (depth * 100.0, juce::sendNotificationSync);
    }

    // Public so the full edit path is testable without the GUI.
    void refresh()
    {
        for (int i = 0; i < VASynthProcessor::kModSlots; ++i)
        {
            const auto s = proc.getModSlot (-1, i);
            auto& r = rows[(std::size_t) i];
            r.src->setSelectedId (s.source + 1, juce::dontSendNotification);
            r.dst->setSelectedId (s.dest + 1,   juce::dontSendNotification);
            r.depth->setValue (s.depth * 100.0, juce::dontSendNotification);
            const bool live = s.source != ModMatrix::SrcNone && s.dest != ModMatrix::DstNone;
            r.depth->setEnabled (live);
            r.depth->setAlpha (live ? 1.0f : 0.4f);
        }
        repaint();
    }

private:
    static void fill (juce::ComboBox& box, const juce::StringArray& names)
    {
        for (int i = 0; i < names.size(); ++i) box.addItem (names[i], i + 1);   // item id = enum + 1
        box.setSelectedId (1, juce::dontSendNotification);
    }
    void timerCallback() override { repaint(); }

    struct Row
    {
        std::unique_ptr<juce::ComboBox> src, dst;
        std::unique_ptr<juce::Slider>   depth;
        juce::TextButton                remove;
        juce::Rectangle<int>            depthArea;
    };

    VASynthProcessor& proc;
    std::array<Row, VASynthProcessor::kModSlots> rows;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModMatrixPanel)
};
