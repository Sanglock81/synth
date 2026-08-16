// Synth — Copyright © 2026 John L Farmer. Licensed under AGPLv3; see LICENSE.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "VASynthLookAndFeel.h"
#include "GuideContent.h"
#include <vector>

// ============================================================================
// Section guide overlay (#133): a menu-driven reference, one section at a time.
// The `?` menu picks a section; this overlay SPOTLIGHTS it (dims the rest of the
// panel), drops NUMBERED markers on its controls, and shows a side card with the
// section's role intro then the numbered entries (NAME / what / how). Transient:
// never persisted, never touches audio. Close: Esc, a tap on the dimmed area, or
// the card's X. Navigation is reopening the menu (no next/prev, no tour state).
//
// The editor computes the section rect + each marker's rect (in overlay coords) and
// hands them in via show(); this component only draws + dismisses.
// ============================================================================
class GuideOverlay : public juce::Component
{
public:
    struct Marker { int number; juce::Rectangle<int> rect; };

    GuideOverlay()
    {
        setWantsKeyboardFocus (false);
        setVisible (false);
    }

    std::function<void()> onDismiss;

    void show (const guide::Section& section, juce::Rectangle<int> sectionRect,
               std::vector<Marker> markerList, bool cardOnRight)
    {
        current   = &section;
        spotlight = sectionRect;
        markers   = std::move (markerList);
        rightSide = cardOnRight;
        setVisible (true);
        toFront (false);
        repaint();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        // Anywhere outside the card (incl. the X and the dimmed area) closes.
        if (! cardBounds().contains (e.getPosition()) || closeButton().contains (e.getPosition()))
            if (onDismiss) onDismiss();
    }

    void paint (juce::Graphics& g) override
    {
        if (current == nullptr) return;

        // --- scrim everywhere EXCEPT the spotlit section (4 strips around it) ---
        const auto full = getLocalBounds();
        const auto s = spotlight;
        const juce::Colour scrim = juce::Colours::black.withAlpha (0.74f);
        g.setColour (scrim);
        g.fillRect (full.withBottom (s.getY()));                                   // top
        g.fillRect (full.withTop (s.getBottom()));                                 // bottom
        g.fillRect (juce::Rectangle<int> (full.getX(), s.getY(), s.getX() - full.getX(), s.getHeight()));           // left
        g.fillRect (juce::Rectangle<int> (s.getRight(), s.getY(), full.getRight() - s.getRight(), s.getHeight()));  // right

        // spotlight frame
        g.setColour (VASynthLookAndFeel::accent().withAlpha (0.9f));
        g.drawRoundedRectangle (s.toFloat().reduced (1.0f), 8.0f, 2.0f);

        // --- numbered markers on the controls ---
        for (auto& m : markers) drawMarker (g, m);

        // --- side card ---
        drawCard (g);
    }

private:
    static constexpr int kMarkerR = 8;

    juce::Rectangle<int> cardBounds() const
    {
        const int w = juce::jmin (440, getWidth() - 40);
        const int h = getHeight() - 40;
        const int x = rightSide ? getWidth() - w - 20 : 20;
        return { x, 20, w, h };
    }
    juce::Rectangle<int> closeButton() const
    {
        auto c = cardBounds();
        return { c.getRight() - 34, c.getY() + 10, 24, 24 };
    }

    void drawMarker (juce::Graphics& g, const Marker& m) const
    {
        // A small accent disc riding the control's top-left corner, with a white ring so it reads on
        // any background. Kept small so a dense control row (the oscillators) doesn't get buried.
        const float r = (float) kMarkerR;
        juce::Point<float> centre ((float) m.rect.getX() + 1.0f, (float) m.rect.getY() + 1.0f);
        juce::Rectangle<float> disc (centre.x - r, centre.y - r, 2.0f * r, 2.0f * r);
        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.fillEllipse (disc.translated (0.0f, 1.0f));                 // soft shadow
        g.setColour (VASynthLookAndFeel::accent());
        g.fillEllipse (disc);
        g.setColour (juce::Colours::white.withAlpha (0.9f));
        g.drawEllipse (disc, 1.0f);
        g.setColour (juce::Colours::black);
        g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
        g.drawText (juce::String (m.number), disc.toNearestInt(), juce::Justification::centred, false);
    }

    void drawCard (juce::Graphics& g) const
    {
        auto card = cardBounds();
        g.setColour (VASynthLookAndFeel::panelLight());
        g.fillRoundedRectangle (card.toFloat(), 10.0f);
        g.setColour (VASynthLookAndFeel::accent().withAlpha (0.6f));
        g.drawRoundedRectangle (card.toFloat().reduced (1.0f), 10.0f, 1.4f);

        // close X
        auto x = closeButton();
        g.setColour (VASynthLookAndFeel::dim());
        g.setFont (juce::Font (juce::FontOptions (18.0f, juce::Font::bold)));
        g.drawText ("X", x, juce::Justification::centred, false);

        auto r = card.reduced (20, 16);
        r.removeFromRight (28);   // keep clear of the X

        g.setColour (VASynthLookAndFeel::ink());
        g.setFont (juce::Font (juce::FontOptions (19.0f, juce::Font::bold)));
        g.drawText (current->title, r.removeFromTop (28), juce::Justification::centredLeft, false);
        r.removeFromTop (4);

        g.setColour (VASynthLookAndFeel::dim());
        g.setFont (juce::Font (juce::FontOptions (12.5f)));
        {
            juce::GlyphArrangement ga;
            const int introH = 58;
            ga.addFittedText (g.getCurrentFont(), current->intro, (float) r.getX(), (float) r.getY(),
                              (float) r.getWidth(), (float) introH, juce::Justification::topLeft, 3, 1.0f);
            ga.draw (g);
            r.removeFromTop (introH);
        }

        g.setColour (VASynthLookAndFeel::accent().withAlpha (0.35f));
        g.fillRect (r.removeFromTop (1).withTrimmedRight (0));
        r.removeFromTop (8);

        if (! current->covered || current->entries.empty())
        {
            g.setColour (VASynthLookAndFeel::dim());
            g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::italic)));
            g.drawText ("Section guide coming soon.", r.removeFromTop (24), juce::Justification::topLeft, false);
            return;
        }

        // Entries: number badge + NAME, then what, then how. Compact so the densest section (FX, 15
        // controls) fits without dropping any (drawFittedText shrinks rather than clips).
        const int perEntry = 47;
        int n = 1;
        for (auto& e : current->entries)
        {
            if (r.getHeight() < perEntry) break;      // ran out of room -> stop (docs/guide.md has the rest)
            auto row = r.removeFromTop (perEntry);

            auto badge = row.removeFromLeft (24).removeFromTop (17);
            g.setColour (VASynthLookAndFeel::accent());
            g.fillEllipse (badge.withWidth (17).toFloat());
            g.setColour (juce::Colours::black);
            g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
            g.drawText (juce::String (n), badge.withWidth (17), juce::Justification::centred, false);

            auto text = row;
            g.setColour (VASynthLookAndFeel::ink());
            g.setFont (juce::Font (juce::FontOptions (12.5f, juce::Font::bold)));
            g.drawText (e.name, text.removeFromTop (16), juce::Justification::topLeft, false);
            g.setColour (VASynthLookAndFeel::ink().withAlpha (0.85f));
            g.setFont (juce::Font (juce::FontOptions (11.5f)));
            g.drawFittedText (e.what, text.removeFromTop (15), juce::Justification::topLeft, 1, 0.85f);
            g.setColour (VASynthLookAndFeel::dim());
            g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::italic)));
            g.drawFittedText (e.how, text.removeFromTop (15), juce::Justification::topLeft, 1, 0.85f);
            ++n;
        }
    }

    const guide::Section* current = nullptr;
    juce::Rectangle<int> spotlight;
    std::vector<Marker> markers;
    bool rightSide = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GuideOverlay)
};
