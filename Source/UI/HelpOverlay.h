#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "VASynthLookAndFeel.h"

// ============================================================================
// Help overlay (R2): a keyboard + gesture cheat-sheet toggled with '?' (Shift+/).
// A dim full-panel scrim with a centred card; tap anywhere or press '?'/Esc to
// dismiss. Refuses keyboard focus so QWERTY note input is never starved.
// ============================================================================

class HelpOverlay : public juce::Component
{
public:
    HelpOverlay()
    {
        setWantsKeyboardFocus (false);
        setVisible (false);
    }

    std::function<void()> onDismiss;

    void mouseDown (const juce::MouseEvent&) override { if (onDismiss) onDismiss(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black.withAlpha (0.72f));

        auto card = getLocalBounds().withSizeKeepingCentre (juce::jmin (620, getWidth() - 60),
                                                            juce::jmin (520, getHeight() - 60));
        g.setColour (VASynthLookAndFeel::panelLight());
        g.fillRoundedRectangle (card.toFloat(), 10.0f);
        g.setColour (VASynthLookAndFeel::accent().withAlpha (0.6f));
        g.drawRoundedRectangle (card.toFloat().reduced (1.0f), 10.0f, 1.4f);

        auto r = card.reduced (26, 20);
        g.setColour (VASynthLookAndFeel::ink());
        g.setFont (juce::Font (juce::FontOptions (20.0f, juce::Font::bold)));
        g.drawText ("Keyboard & touch", r.removeFromTop (34), juce::Justification::centredLeft, false);
        r.removeFromTop (6);

        struct Row { const char* k; const char* d; };
        static const Row rows[] {
            { "A W S E D ...", "play notes (QWERTY musical keyboard)" },
            { "Z  /  X",       "octave down / up" },
            { "C V B N M , .", "hold a chord modifier (Maj Min Sus4 Sus2 Dim Dom7 7th)" },
            { "tap a knob / fader", "grab + adjust the value" },
            { "double-tap", "type an exact value (numeric entry)" },
            { "right-click / long-press", "MIDI-learn this control" },
            { "LINK button, then tap a control", "route the armed mod source to it (drag ~2 s to set depth)" },
            { "tap an FX name bar", "toggle that effect on / off" },
            { "drag an FX name bar", "reorder the FX chain" },
            { "tap a part cell", "assign / edit a locked part (kit editor)" },
            { "F11", "fullscreen (standalone)" },
            { "F12", "audio-health debug overlay" },
            { "?  (Shift + /)", "show / hide this help" },
        };
        for (auto& row : rows)
        {
            auto line = r.removeFromTop (40);
            auto key  = line.removeFromLeft (200);
            g.setColour (VASynthLookAndFeel::accent());
            g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.5f, juce::Font::bold)));
            g.drawText (row.k, key, juce::Justification::centredLeft, false);
            g.setColour (VASynthLookAndFeel::ink());
            g.setFont (juce::Font (juce::FontOptions (13.5f)));
            g.drawText (row.d, line, juce::Justification::centredLeft, true);
        }

        g.setColour (VASynthLookAndFeel::dim());
        g.setFont (juce::Font (juce::FontOptions (12.0f)));
        g.drawText ("tap anywhere to close", card.reduced (26, 16), juce::Justification::bottomRight, false);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HelpOverlay)
};
