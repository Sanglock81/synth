#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <algorithm>
#include "VASynthLookAndFeel.h"
#include "../PluginProcessor.h"

// ============================================================================
// INPUTS routing dialog (7C + Part B). A modal component (no focus leaks — same
// discipline as the Save dialog) listing every playing surface: QWERTY first, then
// each MIDI input by name. Per surface: a routing choice (Live / Part 1-3), a preset
// picker for the assigned locked part, a live-activity dot, and an expandable SPLIT
// editor — key-range zones as a segmented bar with per-zone part/preset/transpose,
// add / remove split, split-by-play (press a key to seam), and reset-to-default.
// A bottom bar resets all routing and saves/loads MULTI layouts (routing included).
// ============================================================================

class InputsDialog : public juce::Component,
                     private juce::Timer
{
public:
    explicit InputsDialog (VASynthProcessor& p) : proc (p)
    {
        setWantsKeyboardFocus (false);
        buildSurfaceList();

        list = std::make_unique<ListContent>();
        for (auto& name : surfaceNames)
        {
            auto row = std::make_unique<SurfaceRow> (proc, name, [this] { relayout(); });
            list->addAndMakeVisible (*row);
            rows.push_back (std::move (row));
        }
        scroll.setViewedComponent (list.get(), false);
        scroll.setScrollBarsShown (true, false);
        addAndMakeVisible (scroll);

        resetAll.setButtonText ("Reset all routing");
        resetAll.setWantsKeyboardFocus (false);
        resetAll.onClick = [this] { proc.resetAllRouting(); for (auto& r : rows) r->refresh(); relayout(); };
        addAndMakeVisible (resetAll);

        saveMulti.setButtonText ("Save MULTI");
        saveMulti.setWantsKeyboardFocus (false);
        saveMulti.onClick = [this] { showSaveMulti(); };
        addAndMakeVisible (saveMulti);

        loadMulti.setTextWhenNothingSelected ("Load MULTI");
        loadMulti.setWantsKeyboardFocus (false);
        loadMulti.onChange = [this]
        {
            const auto n = loadMulti.getText();
            if (n.isNotEmpty()) { proc.loadMulti (n); for (auto& r : rows) r->refresh(); relayout(); }
            loadMulti.setSelectedId (0, juce::dontSendNotification);
        };
        refreshMultiList();
        addAndMakeVisible (loadMulti);

        setSize (640, 560);
        startTimerHz (12);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (VASynthLookAndFeel::panel());
        g.setColour (VASynthLookAndFeel::ink());
        g.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
        g.drawText ("INPUTS  -  route each surface to a part (SPLIT for key ranges)",
                    getLocalBounds().removeFromTop (34).reduced (14, 0), juce::Justification::centredLeft, false);
        g.setColour (VASynthLookAndFeel::dim());
        g.setFont (juce::Font (juce::FontOptions (11.5f)));
        g.drawText ("MULTI saves the whole layout (parts + splits + routing).",
                    juce::Rectangle<int> (14, getHeight() - 58, getWidth() - 28, 16),
                    juce::Justification::centredLeft, false);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        r.removeFromTop (38);
        auto bottom = r.removeFromBottom (58);
        scroll.setBounds (r.reduced (8, 0));
        relayout();

        bottom.removeFromTop (18);
        auto bar = bottom.reduced (14, 0);
        resetAll.setBounds (bar.removeFromLeft (150).reduced (0, 4));
        loadMulti.setBounds (bar.removeFromRight (150).reduced (0, 4));
        bar.removeFromRight (8);
        saveMulti.setBounds (bar.removeFromRight (110).reduced (0, 4));
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

    // ---- test / integration hooks ------------------------------------------
    // The action a surface row performs (route -> part, bake the preset). Public so the
    // full dialog path is integration-testable without driving the GUI.
    void applyRouting (const juce::String& surface, int part, const juce::String& preset)
    {
        proc.setSurfaceRouting (surface, part);
        if (part >= 1 && preset.isNotEmpty() && preset != "(part preset)") proc.setPartPreset (part, preset);
    }
    int          numRows() const { return (int) surfaceNames.size(); }
    juce::String rowName (int i) const { return (i >= 0 && i < (int) surfaceNames.size()) ? surfaceNames[(std::size_t) i] : juce::String(); }
    void         expandSurface (const juce::String& name)   // open a row's SPLIT editor (docs/tests)
    {
        for (auto& r : rows) if (r->name == name) { r->expanded = true; r->rebuild(); }
        relayout();
    }

    // Human-readable note name (e.g. "C3"), middle C = C3 (octaveForMiddleC = 3).
    static juce::String noteName (int n) { return juce::MidiMessage::getMidiNoteName (n, true, true, 3); }
    static juce::Colour  partColour (int part)
    {
        switch (part) { case 1: return juce::Colour (0xffe0733a); case 2: return juce::Colour (0xff3a9ce0);
                        case 3: return juce::Colour (0xffb060d0); default: return VASynthLookAndFeel::accent(); }
    }

private:
    static constexpr int rowH = 40;

    // Fills the whole vertical stack inside the viewport.
    struct ListContent : public juce::Component { void paint (juce::Graphics&) override {} };

    // Populate a preset ComboBox: Init + factory library.
    static void fillPresets (VASynthProcessor& proc, juce::ComboBox& cb)
    {
        cb.clear (juce::dontSendNotification);
        cb.setTextWhenNothingSelected ("(part preset)");
        cb.addItem ("Init", 1);
        int id = 2;
        for (auto& fp : proc.factoryPresetLibrary().all()) cb.addItem (fp.name, id++);
    }

    // ---- one surface: header line + optional expanded zone editor ----------
    struct SurfaceRow : public juce::Component
    {
        SurfaceRow (VASynthProcessor& p, juce::String nm, std::function<void()> relayoutCb)
            : proc (p), name (std::move (nm)), relayout (std::move (relayoutCb))
        {
            setWantsKeyboardFocus (false);

            route.setWantsKeyboardFocus (false);
            route.addItem ("Live", 1); route.addItem ("Part 1", 2); route.addItem ("Part 2", 3); route.addItem ("Part 3", 4);
            route.onChange = [this]
            {
                proc.setSurfaceRouting (name, route.getSelectedId() - 1);   // collapses any split
                refresh(); if (relayout) relayout();
            };
            addAndMakeVisible (route);

            fillPresets (proc, preset);
            preset.setWantsKeyboardFocus (false);
            preset.onChange = [this] { const int prt = route.getSelectedId() - 1;
                                       if (prt >= 1) proc.setPartPreset (prt, preset.getText()); };
            addAndMakeVisible (preset);

            splitBtn.setWantsKeyboardFocus (false);
            splitBtn.setButtonText ("SPLIT");
            splitBtn.onClick = [this] { expanded = ! expanded; rebuild(); if (relayout) relayout(); };
            addAndMakeVisible (splitBtn);

            refresh();
        }

        int desiredHeight() const { return rowH + (expanded ? zoneEditorHeight() : 0); }
        int zoneEditorHeight() const { return 30 + (int) zoneRows.size() * 30 + 34; }

        void paint (juce::Graphics& g) override
        {
            // activity dot + surface name on the header line
            const bool active = lastSeen != proc.surfaceActivity (name) || blink > 0;
            g.setColour (active ? VASynthLookAndFeel::accent() : VASynthLookAndFeel::track());
            g.fillEllipse (10.0f, rowH / 2 - 5.0f, 10.0f, 10.0f);
            g.setColour (VASynthLookAndFeel::ink());
            g.setFont (juce::Font (juce::FontOptions (13.0f)));
            g.drawText (name, 30, 0, 150, rowH, juce::Justification::centredLeft, true);

            if (! expanded) return;

            // segmented zone bar
            auto bar = juce::Rectangle<int> (30, rowH + 2, getWidth() - 60, 22);
            g.setColour (VASynthLookAndFeel::track());
            g.fillRoundedRectangle (bar.toFloat(), 3.0f);
            auto z = proc.getSurfaceZones (name);
            if (z.empty()) z = { VASynthProcessor::Zone{} };
            for (auto& seg : z)
            {
                const int x0 = bar.getX() + bar.getWidth() * seg.loNote / 128;
                const int x1 = bar.getX() + bar.getWidth() * (seg.hiNote + 1) / 128;
                auto cell = juce::Rectangle<int> (x0, bar.getY(), juce::jmax (2, x1 - x0), bar.getHeight()).reduced (1, 1);
                g.setColour (partColour (seg.part).withAlpha (0.85f));
                g.fillRoundedRectangle (cell.toFloat(), 2.0f);
                g.setColour (juce::Colours::black.withAlpha (0.85f));
                g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
                g.drawText (seg.part == 0 ? "LIVE" : ("P" + juce::String (seg.part)), cell, juce::Justification::centred, false);
            }
        }

        void resized() override
        {
            auto header = getLocalBounds().removeFromTop (rowH);
            header.removeFromLeft (185);
            route.setBounds  (header.removeFromLeft (95).reduced (2, 6));
            preset.setBounds (header.removeFromLeft (160).reduced (2, 6));
            splitBtn.setBounds (header.removeFromLeft (70).reduced (2, 6));

            if (! expanded) return;
            auto ed = getLocalBounds();
            ed.removeFromTop (rowH + 28);                    // header + bar
            for (auto& zr : zoneRows) zr->setBounds (ed.removeFromTop (30).reduced (30, 2));
            auto btns = ed.removeFromTop (34).reduced (30, 4);
            addSplit.setBounds (btns.removeFromLeft (90));  btns.removeFromLeft (6);
            byPlay.setBounds   (btns.removeFromLeft (120)); btns.removeFromLeft (6);
            resetSurf.setBounds (btns.removeFromLeft (110));
        }

        // Rebuild the dynamic zone-editor controls from the processor's current zones.
        void rebuild()
        {
            zoneRows.clear();
            addSplit.setVisible (false); byPlay.setVisible (false); resetSurf.setVisible (false);

            if (expanded)
            {
                auto z = proc.getSurfaceZones (name);
                if (z.empty()) z = { VASynthProcessor::Zone{} };
                for (int i = 0; i < (int) z.size(); ++i)
                {
                    auto zr = std::make_unique<ZoneRow> (proc, name, i, [this] { rebuild(); if (relayout) relayout(); });
                    addAndMakeVisible (*zr);
                    zoneRows.push_back (std::move (zr));
                }
                if (! addSplitInit)
                {
                    for (auto* b : { &addSplit, &byPlay, &resetSurf }) { b->setWantsKeyboardFocus (false); addAndMakeVisible (*b); }
                    addSplit.setButtonText ("+ Split");
                    addSplit.onClick = [this] { proc.addSurfaceSplit (name, midSeam()); rebuild(); if (relayout) relayout(); };
                    byPlay.setButtonText ("Split by play");
                    byPlay.setClickingTogglesState (true);
                    byPlay.onClick = [this] { armSeq = proc.surfaceActivity (name); };
                    resetSurf.setButtonText ("Reset surface");
                    resetSurf.onClick = [this] { proc.resetSurfaceZones (name); refresh(); if (relayout) relayout(); };
                    addSplitInit = true;
                }
                addSplit.setVisible (true); byPlay.setVisible (true); resetSurf.setVisible (true);
            }
            splitBtn.setToggleState (expanded, juce::dontSendNotification);
            resized();
        }

        // Reflect the processor's routing back into the header controls.
        void refresh()
        {
            const bool split = proc.surfaceHasSplit (name);
            route.setEnabled (! split);                      // a split is edited in the zone list, not the combo
            route.setSelectedId (split ? 0 : proc.getSurfaceRouting (name) + 1, juce::dontSendNotification);
            const int prt = proc.getSurfaceRouting (name);
            preset.setEnabled (! split && prt >= 1);
            if (prt >= 1) preset.setText (proc.getPartPreset (prt), juce::dontSendNotification);
            rebuild();
        }

        int midSeam() const
        {
            auto z = proc.getSurfaceZones (name);
            if (z.empty()) return 60;
            const auto& big = *std::max_element (z.begin(), z.end(),
                              [] (auto& a, auto& b) { return (a.hiNote - a.loNote) < (b.hiNote - b.loNote); });
            return (big.loNote + big.hiNote) / 2 + 1;
        }

        // Called by the dialog timer: activity blink + split-by-play capture.
        void tick()
        {
            const auto now = proc.surfaceActivity (name);
            if (now != lastSeen) { lastSeen = now; blink = 3; } else if (blink > 0) --blink;

            if (byPlay.getToggleState() && now != armSeq)
            {
                const int n = proc.lastNoteForSurface (name);
                if (n >= 0) { proc.addSurfaceSplit (name, n); byPlay.setToggleState (false, juce::dontSendNotification);
                              rebuild(); if (relayout) relayout(); }
            }
            repaint();
        }

        VASynthProcessor& proc;
        juce::String name;
        std::function<void()> relayout;
        juce::ComboBox route, preset;
        juce::TextButton splitBtn, addSplit, byPlay, resetSurf;
        bool expanded = false, addSplitInit = false;
        std::uint32_t lastSeen = 0, armSeq = 0;
        int blink = 0;

        // ---- one zone within a surface ------------------------------------
        struct ZoneRow : public juce::Component
        {
            ZoneRow (VASynthProcessor& p, juce::String surf, int idx, std::function<void()> structuralCb)
                : proc (p), name (std::move (surf)), index (idx), onStructural (std::move (structuralCb))
            {
                setWantsKeyboardFocus (false);
                part.setWantsKeyboardFocus (false);
                part.addItem ("Live", 1); part.addItem ("Part 1", 2); part.addItem ("Part 2", 3); part.addItem ("Part 3", 4);
                part.onChange = [this] { edit ([this] (VASynthProcessor::Zone& z) { z.part = part.getSelectedId() - 1; }); refreshPreset(); };
                addAndMakeVisible (part);

                fillPresets (proc, preset);
                preset.setWantsKeyboardFocus (false);
                preset.onChange = [this] { const int prt = part.getSelectedId() - 1; if (prt >= 1) proc.setPartPreset (prt, preset.getText()); };
                addAndMakeVisible (preset);

                transpose.setWantsKeyboardFocus (false);
                transpose.setSliderStyle (juce::Slider::IncDecButtons);
                transpose.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 40, 22);
                transpose.setRange (-24, 24, 1);
                transpose.onValueChange = [this] { edit ([this] (VASynthProcessor::Zone& z) { z.transpose = (int) transpose.getValue(); }); };
                addAndMakeVisible (transpose);

                remove.setWantsKeyboardFocus (false);
                remove.setButtonText ("x");
                remove.onClick = [this] { proc.removeSurfaceSplit (name, index); if (onStructural) onStructural(); };
                addAndMakeVisible (remove);

                load();
            }

            void load()
            {
                auto z = proc.getSurfaceZones (name);
                if (index >= (int) z.size()) return;
                const auto& me = z[(std::size_t) index];
                part.setSelectedId (me.part + 1, juce::dontSendNotification);
                transpose.setValue (me.transpose, juce::dontSendNotification);
                refreshPreset();
            }
            void refreshPreset()
            {
                const int prt = part.getSelectedId() - 1;
                preset.setEnabled (prt >= 1);
                if (prt >= 1) preset.setText (proc.getPartPreset (prt), juce::dontSendNotification);
            }
            // Read-modify-write the whole zone vector so setSurfaceZones re-validates.
            void edit (std::function<void (VASynthProcessor::Zone&)> f)
            {
                auto z = proc.getSurfaceZones (name);
                if (index < (int) z.size()) { f (z[(std::size_t) index]); proc.setSurfaceZones (name, std::move (z)); }
            }

            void paint (juce::Graphics& g) override
            {
                auto z = proc.getSurfaceZones (name);
                if (index >= (int) z.size()) return;
                g.setColour (partColour (z[(std::size_t) index].part));
                g.fillRect (0, 4, 4, getHeight() - 8);
                g.setColour (VASynthLookAndFeel::ink());
                g.setFont (juce::Font (juce::FontOptions (11.0f)));
                g.drawText (noteName (z[(std::size_t) index].loNote) + ".." + noteName (z[(std::size_t) index].hiNote),
                            10, 0, 82, getHeight(), juce::Justification::centredLeft, false);
            }
            void resized() override
            {
                auto r = getLocalBounds(); r.removeFromLeft (90);
                part.setBounds  (r.removeFromLeft (80).reduced (2, 1));
                preset.setBounds (r.removeFromLeft (150).reduced (2, 1));
                transpose.setBounds (r.removeFromLeft (90).reduced (2, 1));
                remove.setBounds (r.removeFromLeft (26).reduced (2, 1));
            }

            VASynthProcessor& proc;
            juce::String name;
            int index;
            std::function<void()> onStructural;
            juce::ComboBox part, preset;
            juce::Slider transpose;
            juce::TextButton remove;
        };

        std::vector<std::unique_ptr<ZoneRow>> zoneRows;
    };

    void buildSurfaceList()
    {
        surfaceNames.clear();
        surfaceNames.add ("QWERTY");
        for (auto& d : juce::MidiInput::getAvailableDevices()) surfaceNames.add (d.name);
    }

    void relayout()
    {
        int y = 0;
        const int w = juce::jmax (560, scroll.getMaximumVisibleWidth());
        for (auto& r : rows) { const int h = r->desiredHeight(); r->setBounds (0, y, w, h); y += h + 2; }
        list->setSize (w, juce::jmax (y, scroll.getHeight()));
    }

    void refreshMultiList()
    {
        loadMulti.clear (juce::dontSendNotification);
        int id = 1;
        for (auto& n : proc.getMultiNames()) loadMulti.addItem (n, id++);
    }

    void showSaveMulti()
    {
        auto* aw = new juce::AlertWindow ("Save MULTI", "Name this layout (parts + splits + routing):",
                                          juce::MessageBoxIconType::NoIcon, this);
        aw->addTextEditor ("name", "");
        aw->addButton ("Save",   1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        aw->enterModalState (true, juce::ModalCallbackFunction::create (
            [this, aw] (int result)
            {
                if (result == 1 && proc.saveMulti (aw->getTextEditorContents ("name"))) refreshMultiList();
            }), true);
    }

    void timerCallback() override { for (auto& r : rows) r->tick(); }

    VASynthProcessor& proc;
    juce::StringArray surfaceNames;
    juce::Viewport scroll;
    std::unique_ptr<ListContent> list;
    std::vector<std::unique_ptr<SurfaceRow>> rows;
    juce::TextButton resetAll, saveMulti;
    juce::ComboBox loadMulti;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InputsDialog)
};
