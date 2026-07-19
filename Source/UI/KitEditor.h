#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "VASynthLookAndFeel.h"
#include "Widgets.h"          // kDragPixelsForFullRange
#include "../PluginProcessor.h"

// ============================================================================
// KIT EDITOR (Sub-phase 1). A modal, functional editor for a part's kit: a 4x4
// pad grid (Launchkey layout) + a selected-pad panel — trigger note (learn-by-
// play), source preset, sounding note(s) (learn-by-play, up to 4 = a chord pad),
// pad level, choke group, and an audition tap. Pads flicker on live triggers.
// Refuses keyboard focus so QWERTY / a controller can drive learn-by-play while
// it is open. Edits apply live via setPartKit; Load/Save use the kit library.
// ============================================================================

class KitEditor : public juce::Component,
                  private juce::Timer
{
public:
    KitEditor (VASynthProcessor& p, int startPart) : proc (p), part (juce::jlimit (1, SynthEngine::maxParts - 1, startPart))
    {
        setWantsKeyboardFocus (false);

        partSel.setWantsKeyboardFocus (false);
        for (int i = 1; i < SynthEngine::maxParts; ++i) partSel.addItem ("Part " + juce::String (i), i);
        partSel.setSelectedId (part, juce::dontSendNotification);
        partSel.onChange = [this] { part = partSel.getSelectedId(); loadFromPart(); };
        addAndMakeVisible (partSel);

        kitLoad.setTextWhenNothingSelected ("Load kit");
        kitLoad.setWantsKeyboardFocus (false);
        kitLoad.onChange = [this]
        {
            const auto n = kitLoad.getText();
            if (n.isNotEmpty()) { def = proc.loadKit (n); apply(); rebuildSelected(); }
            kitLoad.setSelectedId (0, juce::dontSendNotification);
        };
        addAndMakeVisible (kitLoad);
        refreshKitList();

        saveBtn.setButtonText ("Save kit"); saveBtn.setWantsKeyboardFocus (false);
        saveBtn.onClick = [this] { showSave(); };
        addAndMakeVisible (saveBtn);

        // Selected-pad controls
        source.setTextWhenNothingSelected ("(source preset)"); source.setWantsKeyboardFocus (false);
        source.addItem ("Init", 1);
        { int id = 2; for (auto& fp : proc.factoryPresetLibrary().all()) source.addItem (fp.name, id++); }
        source.onChange = [this] { pad().source = source.getText(); apply(); repaint(); };
        addAndMakeVisible (source);

        // I2: load a sample onto the selected pad (an alternative to the source preset).
        loadSample.setButtonText ("Load sample..."); loadSample.setWantsKeyboardFocus (false);
        loadSample.onClick = [this]
        {
            chooser = std::make_unique<juce::FileChooser> (
                "Load a sample into pad " + juce::String (selected + 1),
                juce::File::getSpecialLocation (juce::File::userMusicDirectory), "*.wav;*.aiff;*.aif;*.flac");
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                [this] (const juce::FileChooser& fc)
                {
                    const auto f = fc.getResult();
                    if (f != juce::File() && proc.importPadSample (part, selected, f))
                        { def = proc.getPartKit (part); rebuildSelected(); }
                });
        };
        addAndMakeVisible (loadSample);
        clearSample.setButtonText ("Clear sample"); clearSample.setWantsKeyboardFocus (false);
        clearSample.onClick = [this] { proc.clearPadSample (part, selected); def = proc.getPartKit (part); rebuildSelected(); };
        addAndMakeVisible (clearSample);

        learnTrig.setButtonText ("Learn"); learnTrig.setClickingTogglesState (true); learnTrig.setWantsKeyboardFocus (false);
        learnTrig.onClick = [this] { if (learnTrig.getToggleState()) armSeq = proc.noteSeq(); };
        addAndMakeVisible (learnTrig);

        learnSound.setButtonText ("Learn"); learnSound.setClickingTogglesState (true); learnSound.setWantsKeyboardFocus (false);
        learnSound.onClick = [this] { if (learnSound.getToggleState()) { armSeq = proc.noteSeq(); pad().numSound = 0; } };
        addAndMakeVisible (learnSound);

        for (auto* b : { &trigDown, &trigUp, &clearSound, &audition, &clearPad, &editVoice })
        { b->setWantsKeyboardFocus (false); addAndMakeVisible (*b); }
        trigDown.setButtonText ("-"); trigDown.onClick = [this] { pad().triggerNote = juce::jlimit (0, 127, pad().triggerNote < 0 ? 36 : pad().triggerNote - 1); apply(); repaint(); };
        trigUp.setButtonText ("+");   trigUp.onClick   = [this] { pad().triggerNote = juce::jlimit (0, 127, pad().triggerNote < 0 ? 36 : pad().triggerNote + 1); apply(); repaint(); };
        clearSound.setButtonText ("Clear notes"); clearSound.onClick = [this] { pad().numSound = 1; pad().soundNote[0] = pad().triggerNote < 0 ? 60 : pad().triggerNote; apply(); repaint(); };
        audition.setButtonText ("Audition"); audition.onClick = [this] { auditionPad(); };
        clearPad.setButtonText ("Clear pad"); clearPad.onClick = [this] { def.pads[(std::size_t) selected] = {}; def.pads[(std::size_t) selected].triggerNote = -1; apply(); rebuildSelected(); };
        // Open the FULL synth panel on this pad's voice (Group 4): persist the pad, ask the
        // host to begin pad-edit, and close this dialog so the main panel shows the voice.
        editVoice.setButtonText ("Edit voice"); editVoice.onClick = [this]
        {
            if (pad().triggerNote < 0) return;                 // nothing to edit on an empty pad
            apply();
            const int pt = part, s = selected;
            if (onEditVoice) onEditVoice (pt, s);
            if (auto* w = findParentComponentOfClass<juce::DialogWindow>()) w->exitModalState (0);
        };

        level.setSliderStyle (juce::Slider::LinearHorizontal); level.setWantsKeyboardFocus (false);
        level.setSliderSnapsToMousePosition (false);   // R2 grab mode (no jump on touch)
        level.setMouseDragSensitivity (kDragPixelsForFullRange);   // R2: gentler drag-to-value
        level.setRange (0.0, 2.0, 0.01); level.setTextBoxStyle (juce::Slider::TextBoxRight, true, 44, 20);
        level.setTextBoxIsEditable (false);          // no focusable value editor (QWERTY stays live)
        level.onValueChange = [this] { pad().level = (float) level.getValue(); apply(); };
        addAndMakeVisible (level);

        choke.setWantsKeyboardFocus (false);
        for (int g = 0; g <= 4; ++g) choke.addItem (g == 0 ? "none" : juce::String (g), g + 1);
        choke.onChange = [this] { pad().chokeGroup = choke.getSelectedId() - 1; apply(); repaint(); };
        addAndMakeVisible (choke);

        loadFromPart();
        setSize (660, 560);
        startTimerHz (15);
    }

    static void show (VASynthProcessor& proc, juce::Component* parent, int part, std::function<void()> onClose,
                      std::function<void(int, int)> onEditVoice = {})
    {
        auto dlg = std::make_unique<KitEditor> (proc, part);
        dlg->onEditVoice = std::move (onEditVoice);
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned (dlg.release());
        o.dialogTitle = "Kit Editor";
        o.dialogBackgroundColour = VASynthLookAndFeel::panel();
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = false;
        if (parent != nullptr) o.componentToCentreAround = parent;
        auto* w = o.launchAsync();
        if (w != nullptr)
            w->enterModalState (true, juce::ModalCallbackFunction::create ([onClose] (int) { if (onClose) onClose(); }), false);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (VASynthLookAndFeel::panel());
        g.setColour (VASynthLookAndFeel::ink());
        g.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
        g.drawText ("KIT: " + (def.name.isNotEmpty() ? def.name : juce::String ("(unnamed)")),
                    juce::Rectangle<int> (14, 0, 150, 32), juce::Justification::centredLeft, true);

        // 4x4 pad grid
        for (int i = 0; i < kMaxKitPads; ++i)
        {
            auto c = padBounds (i).reduced (3);
            const auto& pd = def.pads[(std::size_t) i];
            const bool sel = i == selected;
            const bool lit = flick[(std::size_t) i] > 0;
            g.setColour (lit ? VASynthLookAndFeel::accent()
                             : (pd.triggerNote >= 0 ? VASynthLookAndFeel::track().brighter (0.12f) : VASynthLookAndFeel::track()));
            g.fillRoundedRectangle (c.toFloat(), 5.0f);
            if (sel) { g.setColour (VASynthLookAndFeel::accent()); g.drawRoundedRectangle (c.toFloat().reduced (1.0f), 5.0f, 2.0f); }

            g.setColour (lit ? juce::Colours::black : VASynthLookAndFeel::ink());
            g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
            g.drawText (juce::String (i + 1), c.reduced (5, 3).removeFromTop (14), juce::Justification::topLeft, false);
            if (pd.triggerNote >= 0)
            {
                g.setFont (juce::Font (juce::FontOptions (11.0f)));
                g.drawText (noteName (pd.triggerNote), c.reduced (5), juce::Justification::centred, false);
                // A sample pad shows a "SMPL" tag (accent) instead of the source preset name.
                const bool smp = pd.samplePath.isNotEmpty();
                g.setColour (smp ? VASynthLookAndFeel::accent() : VASynthLookAndFeel::dim());
                g.setFont (juce::Font (juce::FontOptions (9.5f, smp ? juce::Font::bold : juce::Font::plain)));
                g.drawText (smp ? juce::String ("SMPL") : pd.source,
                            c.reduced (4, 3).removeFromBottom (12), juce::Justification::centred, true);
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        for (int i = 0; i < kMaxKitPads; ++i)
            if (padBounds (i).contains (e.getPosition())) { selected = i; rebuildSelected(); return; }
    }

    void resized() override
    {
        auto top = getLocalBounds().removeFromTop (32);
        top.removeFromLeft (170);
        partSel.setBounds (top.removeFromLeft (90).reduced (2, 3));
        kitLoad.setBounds (top.removeFromLeft (130).reduced (2, 3));
        saveBtn.setBounds (top.removeFromLeft (90).reduced (2, 3));

        // pad editor panel lives below the grid
        auto ed = getLocalBounds();
        ed.removeFromTop (36 + gridH());
        ed = ed.reduced (14, 4);
        auto row = [&] (int h) { auto r = ed.removeFromTop (h); ed.removeFromTop (4); return r; };

        auto r1 = row (24); r1.removeFromLeft (70); trigDown.setBounds (r1.removeFromLeft (26));
        trigUp.setBounds (r1.removeFromLeft (26)); r1.removeFromLeft (8); learnTrig.setBounds (r1.removeFromLeft (60));
        auto r2 = row (24); r2.removeFromLeft (70); source.setBounds (r2.removeFromLeft (200));
        r2.removeFromLeft (8); loadSample.setBounds (r2.removeFromLeft (110));
        r2.removeFromLeft (6); clearSample.setBounds (r2.removeFromLeft (100));
        auto r3 = row (24); r3.removeFromLeft (70); learnSound.setBounds (r3.removeFromLeft (60));
        r3.removeFromLeft (8); clearSound.setBounds (r3.removeFromLeft (90));
        auto r4 = row (24); r4.removeFromLeft (70); level.setBounds (r4.removeFromLeft (200));
        r4.removeFromLeft (30); choke.setBounds (r4.removeFromLeft (80));
        auto r5 = row (26); audition.setBounds (r5.removeFromLeft (100)); r5.removeFromLeft (8); clearPad.setBounds (r5.removeFromLeft (100));
        r5.removeFromLeft (8); editVoice.setBounds (r5.removeFromLeft (110));
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        g.setColour (VASynthLookAndFeel::ink());
        g.setFont (juce::Font (juce::FontOptions (12.0f)));
        auto ed = getLocalBounds(); ed.removeFromTop (36 + gridH()); ed = ed.reduced (14, 4);
        auto lbl = [&] (int h, const juce::String& t) { auto r = ed.removeFromTop (h); ed.removeFromTop (4);
                                                        g.drawText (t, r.removeFromLeft (70), juce::Justification::centredLeft, false); };
        lbl (24, "Trigger " + (pad().triggerNote >= 0 ? noteName (pad().triggerNote) : juce::String ("--")));
        lbl (24, "Source");
        lbl (24, soundLabel());
        lbl (24, "Level");
        ed.removeFromTop (30);
    }

private:
    VASynthProcessor::KitPadDef& pad() { return def.pads[(std::size_t) selected]; }

    int  gridH() const { return 4 * padH; }
    juce::Rectangle<int> padBounds (int i) const
    {
        const int col = i % 4, r = i / 4;
        const int gw = (getWidth() - 28) / 4;
        return { 14 + col * gw, 36 + r * padH, gw, padH };
    }

    void loadFromPart()
    {
        def = proc.isPartKit (part) ? proc.getPartKit (part) : VASynthProcessor::KitDefinition{};
        if (def.name.isEmpty()) def.name = "New Kit";
        selected = 0;
        rebuildSelected();
    }

    void rebuildSelected()
    {
        const auto& pd = pad();
        const bool smp = pd.samplePath.isNotEmpty();
        source.setText (pd.source, juce::dontSendNotification);
        source.setEnabled (! smp);                            // a sample supersedes the source preset
        clearSample.setEnabled (smp);
        level.setValue (pd.level, juce::dontSendNotification);
        choke.setSelectedId (juce::jlimit (0, 4, pd.chokeGroup) + 1, juce::dontSendNotification);
        learnTrig.setToggleState (false, juce::dontSendNotification);
        learnSound.setToggleState (false, juce::dontSendNotification);
        resized(); repaint();
    }

    juce::String soundLabel() const
    {
        const auto& pd = def.pads[(std::size_t) selected];
        if (pd.triggerNote < 0) return "Sounding --";
        juce::String s = "Sounding ";
        for (int i = 0; i < juce::jlimit (1, 4, pd.numSound); ++i) s += noteName (pd.soundNote[(std::size_t) i]) + " ";
        return s;
    }

    void apply() { proc.setPartKit (part, def); }

    void auditionPad()
    {
        if (pad().triggerNote < 0) return;
        proc.routeMidi (juce::MidiMessage::noteOn  (1, pad().triggerNote, 1.0f), part);
        proc.routeMidi (juce::MidiMessage::noteOff (1, pad().triggerNote),       part);
    }

    void refreshKitList()
    {
        kitLoad.clear (juce::dontSendNotification);
        int id = 1;
        for (auto& n : proc.getKitNames()) kitLoad.addItem (n, id++);
    }

    void showSave()
    {
        auto* aw = new juce::AlertWindow ("Save Kit", "Name this kit:", juce::MessageBoxIconType::NoIcon, this);
        aw->addTextEditor ("name", def.name);
        aw->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        aw->enterModalState (true, juce::ModalCallbackFunction::create (
            [this, aw] (int result)
            {
                if (result == 1) { def.name = aw->getTextEditorContents ("name"); if (proc.saveKit (def.name, def)) { refreshKitList(); apply(); repaint(); } }
            }), true);
    }

    void timerCallback() override
    {
        // learn-by-play capture
        const auto seq = proc.noteSeq();
        if (seq != armSeq && (learnTrig.getToggleState() || learnSound.getToggleState()))
        {
            const int n = proc.lastAnyNote();
            if (n >= 0)
            {
                if (learnTrig.getToggleState())
                { pad().triggerNote = n; if (pad().numSound < 1) { pad().numSound = 1; pad().soundNote[0] = n; }
                  learnTrig.setToggleState (false, juce::dontSendNotification); }
                else
                { if (pad().numSound < 4) pad().soundNote[(std::size_t) pad().numSound++] = n; }   // append (up to 4)
                armSeq = seq; apply(); rebuildSelected();
            }
        }

        // pad flicker from live triggers
        for (auto& f : flick) if (f > 0) --f;
        const int lt = proc.partLastTrigger (part);
        const auto pa = proc.partActivity (part);
        if (pa != lastPartAct) { lastPartAct = pa; for (int i = 0; i < kMaxKitPads; ++i) if (def.pads[(std::size_t) i].triggerNote == lt) flick[(std::size_t) i] = 3; }
        repaint();
    }

    static juce::String noteName (int n) { return juce::MidiMessage::getMidiNoteName (n, true, true, 3); }

    static constexpr int padH = 56;

    VASynthProcessor& proc;
    VASynthProcessor::KitDefinition def;
    int part = 1, selected = 0;
    std::uint32_t armSeq = 0, lastPartAct = 0;
    std::array<int, kMaxKitPads> flick {};

    juce::ComboBox partSel, kitLoad, source, choke;
    juce::TextButton saveBtn, learnTrig, learnSound, trigDown, trigUp, clearSound, audition, clearPad, editVoice;
    juce::TextButton loadSample, clearSample;                 // I2: per-pad WAV/AIFF/FLAC
    std::unique_ptr<juce::FileChooser> chooser;               // held: launchAsync is non-blocking
    juce::Slider level;

public:
    std::function<void(int part, int pad)> onEditVoice;   // "Edit voice" -> host begins pad edit
private:

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KitEditor)
};
