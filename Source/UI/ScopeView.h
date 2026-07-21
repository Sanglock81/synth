#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include "VASynthLookAndFeel.h"
#include "PanelChrome.h"
#include "../PluginProcessor.h"

// ============================================================================
// Master oscilloscope + spectrum. Reads the processor's RT-safe scope ring
// (post-master mono) on a timer, draws the waveform in the top half and a log-
// frequency FFT in the bottom half. Purely a display — no audio-thread work here.
// Refuses keyboard focus.
// ============================================================================

class ScopeView : public juce::Component,
                  private juce::Timer
{
public:
    explicit ScopeView (VASynthProcessor& p) : proc (p)
    {
        setWantsKeyboardFocus (false);
        setInterceptsMouseClicks (false, false);   // let the backdrop reclaim QWERTY focus
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        const auto tViz = juce::Colour (0xff67c0c8);
        auto area = getLocalBounds();

        // --- scope (top half) ---
        auto sc = chrome::section (g, area.removeFromTop (area.getHeight() / 2), "Scope  -  master", tViz);
        g.setColour (VASynthLookAndFeel::accent());
        juce::Path w;
        const int sw = juce::jmax (1, sc.getWidth());
        for (int x = 0; x < sw; ++x)
        {
            const int idx = juce::jlimit (0, fftSize - 1, x * fftSize / sw);
            // Vertical gain so a typical playing level fills ~70-80% of the scope box, then a
            // tanh SOFT-CLIP folds hot peaks smoothly to the panel edge (never overdrawn).
            const float v = std::tanh (scope[(std::size_t) idx] * kScopeGain);
            const float y = sc.getCentreY() - v * sc.getHeight() * 0.46f;
            if (x == 0) w.startNewSubPath ((float) sc.getX(), y);
            else        w.lineTo ((float) (sc.getX() + x), y);
        }
        g.strokePath (w, juce::PathStrokeType (2.0f));

        // --- spectrum (bottom half) ---
        auto fb = chrome::section (g, area, "Spectrum  -  FFT", tViz);
        const int bw = juce::jmax (1, fb.getWidth() / kBars);
        for (int b = 0; b < kBars; ++b)
        {
            const float h = bars[(std::size_t) b] * fb.getHeight();
            g.setColour (VASynthLookAndFeel::accent().withAlpha (0.85f));
            g.fillRect (juce::Rectangle<float> ((float) (fb.getX() + b * bw + 1),
                                                (float) fb.getBottom() - h, (float) bw - 2.0f, h));
        }
    }

private:
    void timerCallback() override
    {
        proc.readScope (scope.data(), fftSize);

        // FFT of the captured window -> log-spaced, smoothed bars.
        std::copy (scope.begin(), scope.end(), fftData.begin());
        std::fill (fftData.begin() + fftSize, fftData.end(), 0.0f);
        window.multiplyWithWindowingTable (fftData.data(), fftSize);
        fft.performFrequencyOnlyForwardTransform (fftData.data());

        for (int b = 0; b < kBars; ++b)
        {
            // log-spaced bin for this bar (skip DC).
            const float t0 = (float) b / kBars, t1 = (float) (b + 1) / kBars;
            const int lo = juce::jlimit (1, fftSize / 2 - 1, (int) (std::pow (t0, 2.2f) * (fftSize / 2)));
            const int hi = juce::jlimit (lo + 1, fftSize / 2, (int) (std::pow (t1, 2.2f) * (fftSize / 2)));
            float peak = 0.0f;
            for (int i = lo; i < hi; ++i) peak = juce::jmax (peak, fftData[(std::size_t) i]);
            const float db = juce::Decibels::gainToDecibels (peak / (float) (fftSize / 4) + 1.0e-9f);
            const float norm = juce::jlimit (0.0f, 1.0f, (db + 72.0f) / 72.0f);
            auto& bar = bars[(std::size_t) b];
            bar = norm > bar ? norm : bar * 0.72f + norm * 0.28f;   // fast attack, slow decay
        }
        repaint();
    }

    static constexpr float kScopeGain = 3.6f;   // ~0.25 in -> ~0.7 of half-height (tanh soft-clipped)
    static constexpr int kBars    = 44;
    static constexpr int fftOrder = 10;
    static constexpr int fftSize  = 1 << fftOrder;

    VASynthProcessor& proc;
    juce::dsp::FFT fft { fftOrder };
    juce::dsp::WindowingFunction<float> window { (size_t) fftSize, juce::dsp::WindowingFunction<float>::hann };
    std::array<float, fftSize>     scope {};
    std::array<float, fftSize * 2> fftData {};
    std::array<float, kBars>       bars {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScopeView)
};
