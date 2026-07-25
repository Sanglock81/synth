// ============================================================================
// 7A drum presets: character checks (load + sound + bounds are covered for ALL
// presets by test_presets_factory; here we verify the DRUM character specifically).
//   * Kick: percussive (decays to near-silence) with a low fundamental at the note
//     an octave down — and env->pitch gives it a descending pitch (onset above tail).
//   * Closed Hat: highpassed noise (energy concentrated in the highs), very short.
// FFT/zero-cross helpers come from the DSP test util (on the plugin_tests include path).
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "PluginProcessor.h"
#include "test_util.h"
#include <vector>
#include <cmath>

namespace
{
    // Render one held note of a factory preset into a mono capture.
    std::vector<float> renderPreset (VASynthProcessor& p, const juce::String& preset, int note, double seconds)
    {
        p.loadFactoryPreset (preset);
        const int total = (int) (48000.0 * seconds);
        std::vector<float> mono; mono.reserve ((size_t) total);
        juce::AudioBuffer<float> buf (2, 256);
        int done = 0;
        while (done < total)
        {
            const int n = std::min (256, total - done);
            buf.setSize (2, n, false, false, true); buf.clear();
            juce::MidiBuffer midi;
            if (done == 0) midi.addEvent (juce::MidiMessage::noteOn (1, note, 1.0f), 0);
            p.processBlock (buf, midi);
            const float* L = buf.getReadPointer (0);
            for (int i = 0; i < n; ++i) mono.push_back (L[i]);
            done += n;
        }
        return mono;
    }

    double rmsRange (const std::vector<float>& x, double sr, double t0, double t1)
    {
        const int a = (int) (sr * t0), b = std::min ((int) (sr * t1), (int) x.size());
        double acc = 0.0; int n = 0;
        for (int i = a; i < b; ++i) { acc += double (x[(size_t) i]) * x[(size_t) i]; ++n; }
        return n ? std::sqrt (acc / n) : 0.0;
    }
}

TEST_CASE ("Kick preset: low descending fundamental, percussive decay", "[plugin][7a][drums][kick]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p; p.prepareToPlay (48000.0, 256);

    auto out = renderPreset (p, "Kick 808", 48, 0.5);   // C3; osc octave -1 -> ~C2 (65 Hz)
    REQUIRE (tu::allFinite (out));

    // Settled fundamental (after the pitch drop) is the low note (~65 Hz).
    const double settled = tu::zeroCrossHz (out, int (48000 * 0.18), int (48000 * 0.25), 48000.0);
    INFO ("kick settled fundamental = " << settled << " Hz");
    REQUIRE (settled > 45.0);
    REQUIRE (settled < 110.0);                          // clearly a low kick, not the raw note

    // env->pitch: the onset carries clear energy ABOVE the settled fundamental (the
    // pitch swept down from ~+22 st), while the settled tail is a pure low tone.
    auto highBandEnergy = [&] (double t0)
    {
        const int start = (int) (48000 * t0);
        std::vector<float> w (2048, 0.0f);
        for (int i = 0; i < 2048 && start + i < (int) out.size(); ++i) w[(size_t) i] = out[(size_t) (start + i)];
        auto mag = tu::magnitudeSpectrum (w);
        const double binHz = 48000.0 / 2048.0;
        double e = 0.0;
        for (size_t k = 1; k < mag.size(); ++k) if (k * binHz > 130.0) e += mag[k] * mag[k];
        return e;
    };
    REQUIRE (highBandEnergy (0.0) > highBandEnergy (0.2) * 5.0);   // pitch drop: onset highs >> tail

    // Percussive: the tail is far quieter than the body (amp sustain 0).
    REQUIRE (rmsRange (out, 48000, 0.0,  0.05) > rmsRange (out, 48000, 0.4, 0.5) * 8.0);
}

TEST_CASE ("Closed hat preset: highpassed noise, very short", "[plugin][7a][drums][hat]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p; p.prepareToPlay (48000.0, 256);

    auto out = renderPreset (p, "Hat Closed", 60, 0.3);
    REQUIRE (tu::allFinite (out));

    // Spectrum of the onset: energy must sit mostly in the highs (highpass @ ~8.5k).
    std::vector<float> win (8192, 0.0f);
    for (int i = 0; i < 8192 && i < (int) out.size(); ++i) win[(size_t) i] = out[(size_t) i];
    auto mag = tu::magnitudeSpectrum (win);
    const double binHz = 48000.0 / 8192.0;
    double low = 0.0, high = 0.0;
    for (size_t k = 1; k < mag.size(); ++k)
        (k * binHz < 4000.0 ? low : high) += mag[k] * mag[k];
    INFO ("hat low(<4k)=" << low << " high(>4k)=" << high);
    REQUIRE (high > low * 4.0);                          // clearly highpassed

    // Very short: essentially silent by 120 ms.
    REQUIRE (rmsRange (out, 48000, 0.0, 0.03) > rmsRange (out, 48000, 0.12, 0.3) * 15.0);
}

// --- 808 kit completion (Inc 1): the new cymbals + the dense-mash torture -----------------
TEST_CASE ("Crash rings long, Splash is short (cymbal decay)", "[plugin][drums][808][cymbal]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p; p.prepareToPlay (48000.0, 256);

    auto crash  = renderPreset (p, "Crash",  57, 3.5);
    auto splash = renderPreset (p, "Splash", 57, 3.5);
    REQUIRE (tu::allFinite (crash));
    REQUIRE (tu::allFinite (splash));

    // Crash still ringing around 1 s; splash essentially gone by then.
    REQUIRE (rmsRange (crash,  48000, 0.9, 1.2) > 0.005);                 // long tail
    REQUIRE (rmsRange (splash, 48000, 0.9, 1.2) < rmsRange (crash, 48000, 0.9, 1.2) * 0.3);
    REQUIRE (rmsRange (splash, 48000, 0.0, 0.1) > 0.02);                  // but splash is audible up front
}

TEST_CASE ("808 kit: a dense 16th mash including the crash stays clean + bounded", "[plugin][drums][808][torture]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 128);          // default scene: P4 = 808 kit, seq target P4

    auto set = [&] (const char* id, float v) { auto* pp = p.apvts.getParameter (id); pp->setValueNotifyingHost (pp->convertTo0to1 (v)); };
    set (ParamID::tempo, 180.0f);            // fast
    for (int row = 0; row < 8; ++row)        // FULL grid: every default drum (incl. crash row) on every 16th
        for (int step = 0; step < 16; ++step) p.setSeqCell (row, step, 1);
    set (ParamID::seqOn, 1.0f);

    float peak = 0.0f; bool finite = true;
    for (int b = 0; b < 700; ++b)            // ~4 bars @ 180 BPM, 128-sample blocks
    {
        juce::AudioBuffer<float> buf (2, 128); buf.clear();
        juce::MidiBuffer m;
        p.processBlock (buf, m);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 128; ++i)
            {
                const float s = buf.getSample (ch, i);
                if (! std::isfinite (s)) finite = false;
                peak = std::max (peak, std::abs (s));
            }
    }
    INFO ("mash peak=" << peak);
    REQUIRE (finite);                        // no NaN/Inf under a dense mash + a long ringing crash
    REQUIRE (peak > 0.05f);                  // it's actually sounding
    REQUIRE (peak <= 1.05f);                 // the safety clipper keeps it bounded — no runaway
}
