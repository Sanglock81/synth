#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "VASynthLookAndFeel.h"
#include "UiText.h"
#include "../ModDestRegistry.h"
#include "../PluginProcessor.h"

// ============================================================================
// MOD MATRIX overlay (#56). A modal panel (same no-focus-leak discipline as the Save /
// INPUTS dialogs) listing the FOCUSED part's 8 routing slots. Each row: a source, an
// arrow, a destination, a bipolar depth slider (gold = positive, cyan = inverted) with a
// numeric read-out, a live-activity dot, and a drawn remove ✕. Routes are usually made by
// the LINK gesture (arm a source, tap a knob); this overlay inspects, re-points, re-depths,
// inverts and deletes them — or builds one from scratch with the two grouped dropdowns.
// The destination list is CATEGORIZED (Osc / Filter / Env / LFO / FX / Part) so the ~40
// targets stay navigable.
// ============================================================================

class ModMatrixPanel : public juce::Component,
                       private juce::Timer
{
public:
    // Flat name lists indexed by enum id (item id = enum + 1) — used by the LINK source menu
    // and by tests. The panel's own dropdowns are built grouped (see fillSource/fillDest).
    static juce::StringArray sourceNames()
    {
        juce::StringArray a { "LFO 1", "LFO 2", "LFO 3", "Mod Env", "Amp Env", "Velocity", "Note",
                              "Mod Wheel", "Pitch Bend", "Random",
                              "Macro 1", "Macro 2", "Macro 3", "Macro 4", "Macro 5", "Macro 6", "Macro 7", "Macro 8" };
        a.insert (0, uitext::u8 ("\xe2\x80\x94"));   // item 0 = "no source" (real em-dash via u8)
        return a;
    }
    static juce::StringArray destNames()
    {
        juce::StringArray a;
        a.add (uitext::u8 ("\xe2\x80\x94"));         // DstNone
        for (int d = 1; d < ModMatrix::kNumDests; ++d)
        {
            const auto n = moddest::nameFor (d);
            a.add (n.isNotEmpty() ? n : uitext::u8 ("\xe2\x80\x94"));
        }
        return a;
    }

    explicit ModMatrixPanel (VASynthProcessor& p) : proc (p)
    {
        setWantsKeyboardFocus (false);

        // Restore-all-macro-defaults (H2): resets the 8 macro->param assignments to the factory map.
        resetMacros.setButtonText ("Restore macro defaults");
        resetMacros.setWantsKeyboardFocus (false);
        resetMacros.onClick = [this] { proc.resetMacroAssignments(); proc.postToast ("Macro assignments restored to defaults"); };
        addAndMakeVisible (resetMacros);

        for (int i = 0; i < VASynthProcessor::kModSlots; ++i)
        {
            auto& r = rows[(std::size_t) i];
            r.src  = std::make_unique<juce::ComboBox>();
            r.dst  = std::make_unique<juce::ComboBox>();
            r.depth = std::make_unique<juce::Slider> (juce::Slider::LinearHorizontal, juce::Slider::NoTextBox);
            fillSource (*r.src);
            fillDest   (*r.dst);
            r.depth->setRange (-100.0, 100.0, 1.0);
            r.depth->setDoubleClickReturnValue (true, 0.0);
            for (auto* c : { (juce::Component*) r.src.get(), (juce::Component*) r.dst.get(), (juce::Component*) r.depth.get() })
                { c->setWantsKeyboardFocus (false); addAndMakeVisible (c); }
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
        setSize (600, kHeaderH + kColsH + VASynthProcessor::kModSlots * kRowH + kFootH);
        startTimerHz (20);   // live modulation dots
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (VASynthLookAndFeel::panel());

        // Title bar — section-header language.
        auto title = getLocalBounds().removeFromTop (kHeaderH);
        g.setColour (VASynthLookAndFeel::panelLight());
        g.fillRect (title);
        g.setColour (VASynthLookAndFeel::ink());
        g.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
        g.drawText ("MOD MATRIX", title.reduced (16, 0), juce::Justification::centredLeft, false);
        g.setColour (VASynthLookAndFeel::accent());
        g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::bold)));
        g.drawText ("PART " + juce::String (proc.editFocus() + 1), title.reduced (16, 0), juce::Justification::centredRight, false);

        // Column headers.
        g.setColour (VASynthLookAndFeel::dim());
        g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
        const auto& c0 = rows[0];
        auto hdr = [&] (juce::Rectangle<int> b, const char* t, juce::Justification j)
        { g.drawText (t, b.withY (kHeaderH + 4).withHeight (kColsH - 6), j, false); };
        if (c0.src)   hdr (c0.src->getBounds(),   "SOURCE",      juce::Justification::centredLeft);
        if (c0.dst)   hdr (c0.dst->getBounds(),   "DESTINATION", juce::Justification::centredLeft);
        if (c0.depth) hdr (c0.depth->getBounds(), "DEPTH",       juce::Justification::centredLeft);

        // Footer hint.
        g.setColour (VASynthLookAndFeel::dim());
        g.setFont (juce::Font (juce::FontOptions (11.5f)));
        g.drawText (uitext::u8 ("Arm LINK and tap a knob to route \xe2\x80\x94 or add one below. Depth is bipolar (drag left to invert)."),
                    juce::Rectangle<int> (16, getHeight() - kFootH + 4, getWidth() - 32 - 190, 16), juce::Justification::centredLeft, false);
    }

    void resized() override
    {
        resetMacros.setBounds (getLocalBounds().removeFromBottom (kFootH).removeFromRight (184).reduced (10, 4));
        auto c = getLocalBounds().reduced (14, 0);
        c.removeFromTop (kHeaderH + kColsH);
        for (int i = 0; i < VASynthProcessor::kModSlots; ++i)
        {
            auto row = c.removeFromTop (kRowH).reduced (0, 4);
            auto& r = rows[(std::size_t) i];
            row.removeFromLeft (14);                                    // activity-dot gutter
            r.src->setBounds  (row.removeFromLeft (120));               row.removeFromLeft (22);   // arrow gap
            r.dst->setBounds  (row.removeFromLeft (128));               row.removeFromLeft (10);
            r.remove.setBounds (row.removeFromRight (24).reduced (1, 3)); row.removeFromRight (6);
            r.numArea = row.removeFromRight (44);                       // numeric depth read-out
            r.depthArea = row;
            r.depth->setBounds (row);
        }
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        for (int i = 0; i < VASynthProcessor::kModSlots; ++i)
        {
            auto& r = rows[(std::size_t) i];
            const auto s = proc.getModSlot (-1, i);
            const bool live = s.source != ModMatrix::SrcNone && s.dest != ModMatrix::DstNone;

            // Arrow between the dropdowns.
            const auto sb = r.src->getBounds();
            g.setColour (live ? VASynthLookAndFeel::accent() : VASynthLookAndFeel::dim().withAlpha (0.5f));
            g.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
            g.drawText (uitext::u8 ("\xe2\x86\x92"), juce::Rectangle<int> (sb.getRight(), sb.getY(), 22, sb.getHeight()),
                        juce::Justification::centred, false);

            if (! live)
            {
                // Empty slot: a faint "+ add route" cue across the (disabled) depth area.
                g.setColour (VASynthLookAndFeel::dim().withAlpha (0.5f));
                g.setFont (juce::Font (juce::FontOptions (11.0f)));
                g.drawText ("+ add route", r.depthArea.toNearestInt(), juce::Justification::centredLeft, false);
                continue;
            }

            // Numeric depth read-out (gold positive, cyan inverted).
            g.setColour (s.depth < 0.0f ? juce::Colour (0xff4bb3c4) : VASynthLookAndFeel::accent());
            g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 11.0f, juce::Font::bold)));
            g.drawText (juce::String (juce::roundToInt (s.depth * 100.0f)) + "%",
                        r.numArea, juce::Justification::centredRight, false);

            // Live-activity dot: brightens with the route's current modulation (block-tier reads
            // the live per-dest offset; voice-tier pulses on the depth magnitude as a fallback).
            const float act = liveActivity (s);
            auto dot = juce::Rectangle<float> (getLocalBounds().reduced (16, 0).toFloat().getX() - 2.0f,
                                               (float) r.depth->getBounds().getCentreY() - 3.0f, 6.0f, 6.0f);
            g.setColour (VASynthLookAndFeel::accentWarm().withAlpha (0.25f + 0.75f * juce::jlimit (0.0f, 1.0f, act)));
            g.fillEllipse (dot);
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
            r.remove.setVisible (live);
        }
        repaint();
    }

private:
    // A drawn ✕ (no font-glyph dependency — see the mojibake defect).
    struct XButton : juce::Button
    {
        XButton() : juce::Button ("remove") {}
        void paintButton (juce::Graphics& g, bool over, bool) override
        {
            auto b = getLocalBounds().toFloat().reduced (getWidth() * 0.32f);
            g.setColour (over ? juce::Colour (0xffd8443a) : VASynthLookAndFeel::dim());
            g.drawLine (b.getX(), b.getY(), b.getRight(), b.getBottom(), 1.8f);
            g.drawLine (b.getX(), b.getBottom(), b.getRight(), b.getY(), 1.8f);
        }
    };

    // Grouped source dropdown (section headings keep item ids intact so setSelectedId/getText work).
    void fillSource (juce::ComboBox& box)
    {
        box.addItem (uitext::u8 ("\xe2\x80\x94"), 1);                    // None (source 0 + 1)
        auto add = [&] (int src) { box.addItem (sourceNames()[src], src + 1); };
        box.addSectionHeading ("LFO");         for (int s = ModMatrix::LFO1; s <= ModMatrix::LFO3; ++s) add (s);
        box.addSectionHeading ("Envelope");    add (ModMatrix::ModEnv); add (ModMatrix::AmpEnv);
        box.addSectionHeading ("Performance"); for (int s = ModMatrix::Velocity; s <= ModMatrix::Random; ++s) add (s);
        box.addSectionHeading ("Macro");       for (int s = ModMatrix::Macro1; s <= ModMatrix::Macro8; ++s) add (s);
        box.setSelectedId (1, juce::dontSendNotification);
    }

    // Grouped destination dropdown, by registry category.
    void fillDest (juce::ComboBox& box)
    {
        box.addItem (uitext::u8 ("\xe2\x80\x94"), 1);                    // None (dest 0 + 1)
        for (int cat = 0; cat < moddest::kNumCategories; ++cat)
        {
            bool headed = false;
            for (auto& e : moddest::table())
                if (e.category == cat)
                {
                    if (! headed) { box.addSectionHeading (moddest::categoryName (cat)); headed = true; }
                    box.addItem (e.name, e.dest + 1);
                }
        }
        box.setSelectedId (1, juce::dontSendNotification);
    }

    float liveActivity (const ModMatrix::Slot& s) const
    {
        if (s.dest >= ModMatrix::kFirstBlockDest)
            return std::abs (proc.blockModOffset (s.dest)) * 4.0f;      // block-tier: live per-dest offset
        return std::abs (s.depth) * 0.6f;                              // voice-tier: fall back to depth magnitude
    }

    void timerCallback() override { repaint(); }

    static constexpr int kHeaderH = 36, kColsH = 18, kRowH = 40, kFootH = 26;

    struct Row
    {
        std::unique_ptr<juce::ComboBox> src, dst;
        std::unique_ptr<juce::Slider>   depth;
        XButton                         remove;
        juce::Rectangle<int>            depthArea, numArea;
    };

    VASynthProcessor& proc;
    juce::TextButton resetMacros;
    std::array<Row, VASynthProcessor::kModSlots> rows;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModMatrixPanel)
};
