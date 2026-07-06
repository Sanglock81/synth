#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

// ============================================================================
// One dark hardware-panel LookAndFeel for the whole UI — no per-widget style
// hacks. High-contrast fader tracks/thumbs, subtle section tints, legible value
// text; readable at arm's length on a touchscreen. Fader thumbs are sized for
// fingers, not mouse pointers.
// ============================================================================

class VASynthLookAndFeel : public juce::LookAndFeel_V4
{
public:
    // Palette.
    static juce::Colour panel()      { return juce::Colour (0xff1c1f24); }
    static juce::Colour panelLight() { return juce::Colour (0xff262a31); }
    static juce::Colour ink()        { return juce::Colour (0xffe8eaed); }
    static juce::Colour dim()        { return juce::Colour (0xff9aa0a8); }
    static juce::Colour accent()     { return juce::Colour (0xff46c9b0); }   // teal
    static juce::Colour accentWarm() { return juce::Colour (0xfff0a04b); }   // amber (learn)
    static juce::Colour track()      { return juce::Colour (0xff10131a); }

    VASynthLookAndFeel()
    {
        setColour (juce::ResizableWindow::backgroundColourId, panel());
        setColour (juce::Slider::textBoxTextColourId, ink());
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Label::textColourId, ink());
        setColour (juce::ComboBox::backgroundColourId, panelLight());
        setColour (juce::ComboBox::textColourId, ink());
        setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff3a3f47));
        setColour (juce::PopupMenu::backgroundColourId, panelLight());
        setColour (juce::PopupMenu::textColourId, ink());
    }

    juce::Font sectionFont()  { return juce::Font (juce::FontOptions (13.0f, juce::Font::bold)); }
    juce::Font labelFont()    { return juce::Font (juce::FontOptions (12.0f)); }
    juce::Font valueFont()    { return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain)); }

    // Vertical fader: recessed track, bright filled portion, chunky finger thumb.
    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float minPos, float maxPos,
                           const juce::Slider::SliderStyle style, juce::Slider& s) override
    {
        if (style != juce::Slider::LinearVertical)
        {
            juce::LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minPos, maxPos, style, s);
            return;
        }

        const float cx = x + width * 0.5f;
        const float trackW = 8.0f;
        juce::Rectangle<float> trackR (cx - trackW * 0.5f, (float) y + 4.0f, trackW, (float) height - 8.0f);

        g.setColour (track());
        g.fillRoundedRectangle (trackR, 4.0f);

        // filled portion from the bottom up to the thumb
        auto filled = trackR;
        filled.setTop (juce::jlimit (trackR.getY(), trackR.getBottom(), sliderPos));
        g.setColour (accent().withAlpha (s.isEnabled() ? 0.95f : 0.4f));
        g.fillRoundedRectangle (filled, 4.0f);

        // thumb — big finger target: >= 56 px wide at default scale.
        const float thumbW = juce::jmin ((float) width - 2.0f, 56.0f);
        const float thumbH = 20.0f;
        juce::Rectangle<float> thumb (cx - thumbW * 0.5f, sliderPos - thumbH * 0.5f, thumbW, thumbH);
        g.setColour (juce::Colour (0xffdfe3e8));
        g.fillRoundedRectangle (thumb, 4.0f);
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.drawRoundedRectangle (thumb, 4.0f, 1.0f);
        g.setColour (accent().darker (0.2f));
        g.fillRect (thumb.reduced (5.0f, 0.0f).withHeight (2.0f).withY (thumb.getCentreY() - 1.0f));
    }

    // Dark rotary knob: recessed track arc, accent value arc, raised body with a
    // pointer. Sized to read on a touchscreen; the whole widget is the finger target.
    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float startAngle, float endAngle,
                           juce::Slider& s) override
    {
        auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (3.0f);
        const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const float cx = bounds.getCentreX(), cy = bounds.getCentreY();
        const float angle = startAngle + sliderPos * (endAngle - startAngle);
        const float lineW = juce::jmax (2.5f, radius * 0.16f);
        const float arcR = radius - lineW * 0.5f;

        juce::Path track;
        track.addCentredArc (cx, cy, arcR, arcR, 0.0f, startAngle, endAngle, true);
        g.setColour (VASynthLookAndFeel::track());
        g.strokePath (track, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        juce::Path val;
        val.addCentredArc (cx, cy, arcR, arcR, 0.0f, startAngle, angle, true);
        g.setColour (accent().withAlpha (s.isEnabled() ? 0.95f : 0.4f));
        g.strokePath (val, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        const float bodyR = radius - lineW * 1.7f;
        g.setColour (panelLight().brighter (0.05f));
        g.fillEllipse (cx - bodyR, cy - bodyR, bodyR * 2.0f, bodyR * 2.0f);
        g.setColour (juce::Colour (0xff3a3f47));
        g.drawEllipse (cx - bodyR, cy - bodyR, bodyR * 2.0f, bodyR * 2.0f, 1.0f);

        const juce::Point<float> tip (cx + std::cos (angle - juce::MathConstants<float>::halfPi) * bodyR * 0.85f,
                                      cy + std::sin (angle - juce::MathConstants<float>::halfPi) * bodyR * 0.85f);
        g.setColour (ink());
        g.drawLine (juce::Line<float> (juce::Point<float> (cx, cy), tip), juce::jmax (2.0f, radius * 0.10f));
    }

    // Fit button text to the button width (shrink the font rather than truncate),
    // so no label is ever clipped at any size.
    void drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool) override
    {
        auto area = b.getLocalBounds().reduced (5, 2);
        auto text = b.getButtonText();
        auto font = getTextButtonFont (b, b.getHeight());
        const float w = juce::GlyphArrangement::getStringWidth (font, text);
        if (w > (float) area.getWidth() && w > 0.0f)
            font.setHeight (juce::jmax (8.0f, font.getHeight() * (float) area.getWidth() / w));
        g.setFont (font);
        g.setColour (b.findColour (b.getToggleState() ? juce::TextButton::textColourOnId
                                                      : juce::TextButton::textColourOffId)
                        .withMultipliedAlpha (b.isEnabled() ? 1.0f : 0.5f));
        g.drawText (text, area, juce::Justification::centred, false);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VASynthLookAndFeel)
};

// ---------------------------------------------------------------------------
// A titled panel section with a subtle tint and outline. Children are laid out
// by the owner; the section just draws the frame + header.
class Section : public juce::Component
{
public:
    Section (juce::String titleText, juce::Colour tintColour)
        : title (std::move (titleText)), tint (tintColour) {}

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (2.0f);
        g.setColour (VASynthLookAndFeel::panelLight().interpolatedWith (tint, 0.10f));
        g.fillRoundedRectangle (r, 7.0f);
        g.setColour (tint.withAlpha (0.55f));
        g.drawRoundedRectangle (r, 7.0f, 1.2f);

        g.setColour (VASynthLookAndFeel::ink());
        g.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
        g.drawText (title.toUpperCase(), getLocalBounds().removeFromTop (headerHeight).withTrimmedLeft (10),
                    juce::Justification::centredLeft, false);
    }

    // Content area below the header (owner lays out children here).
    juce::Rectangle<int> contentBounds() const
    {
        return getLocalBounds().withTrimmedTop (headerHeight).reduced (8, 6);
    }

    // Lay out child controls left to right; each control's "layoutFlex" property
    // sets its relative width (faders 1.0, segmented controls wider).
    void resized() override
    {
        auto kids = getChildren();
        if (kids.isEmpty()) return;
        juce::FlexBox fb;
        fb.flexDirection = juce::FlexBox::Direction::row;
        for (auto* c : kids)
        {
            const float flex = (float) (double) c->getProperties().getWithDefault ("layoutFlex", 1.0);
            fb.items.add (juce::FlexItem (*c).withFlex (flex).withMargin (3.0f));
        }
        fb.performLayout (contentBounds());
    }

    static constexpr int headerHeight = 24;

private:
    juce::String title;
    juce::Colour tint;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Section)
};
