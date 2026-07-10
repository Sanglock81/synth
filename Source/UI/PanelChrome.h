#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "VASynthLookAndFeel.h"

// ============================================================================
// Shared panel chrome for the R2 layout: the signed-off "filled tint header"
// section frame and the inset sub-box (one per oscillator / LFO). Free functions
// so every panel (centre sections, part rail, bottom zones, scope) draws an
// identical frame, and the layout math (content rect) is computed the SAME way in
// paint() and resized() from one source of truth.
// ============================================================================

namespace chrome
{
    inline juce::Colour onTint() { return juce::Colour (0xff0e1319); }   // near-black text on a lit tint

    inline constexpr int kHeaderH = 24;

    // Content rect of a section given its full bounds — the region children fill.
    inline juce::Rectangle<int> sectionContent (juce::Rectangle<int> r, int headH = kHeaderH)
    {
        return r.withTrimmedTop (headH).reduced (6, 5);
    }

    // Draw a section frame (rounded body + filled tint header bar with the title)
    // and return the content rect for children. Mirrors sectionContent().
    inline juce::Rectangle<int> section (juce::Graphics& g, juce::Rectangle<int> r,
                                         const juce::String& title, juce::Colour tint, int headH = kHeaderH)
    {
        g.setColour (VASynthLookAndFeel::panelLight().darker (0.12f));
        g.fillRoundedRectangle (r.toFloat(), 6.0f);
        auto head = r.removeFromTop (headH);
        g.setColour (tint);
        g.fillRoundedRectangle (head.toFloat(), 6.0f);
        g.fillRect (head.withTop (head.getCentreY()));            // square off the bottom of the bar
        g.setColour (onTint());
        g.setFont (juce::Font (juce::FontOptions (12.5f, juce::Font::bold)));
        g.drawText ("  " + title.toUpperCase(), head, juce::Justification::centredLeft, false);
        return r.reduced (6, 5);
    }

    // Inset box for one oscillator / LFO; returns its content rect.
    inline juce::Rectangle<int> subBox (juce::Graphics& g, juce::Rectangle<int> r, juce::Colour tint)
    {
        g.setColour (VASynthLookAndFeel::panel());
        g.fillRoundedRectangle (r.toFloat(), 5.0f);
        g.setColour (tint.withAlpha (0.4f));
        g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 5.0f, 1.0f);
        return r.reduced (6, 5);
    }

    inline juce::Rectangle<int> subBoxContent (juce::Rectangle<int> r) { return r.reduced (6, 5); }

    // A small centred caption under a knob/fader (for controls we lay out by hand).
    inline void caption (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& t)
    {
        g.setColour (VASynthLookAndFeel::dim());
        g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
        g.drawText (t, r, juce::Justification::centred, false);
    }
}
