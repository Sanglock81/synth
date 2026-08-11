// ============================================================================
// Bass Rework (Preset Expansion III, Batch A) audition + loudness-report harness.
//
// Both tests are HIDDEN (leading-dot tags) so they never run in the normal ctest
// gate — they are triage/authoring tools invoked explicitly:
//
//   plugin_tests "[.bassreport]"   -> prints every patch's sustained-window dBFS,
//                                     the bank median, and each Bass patch's delta,
//                                     so patch_trim can be tuned to the +/-4 dB gate.
//   plugin_tests "[.audition]"     -> renders a fixed bass phrase per Bass-category
//                                     patch to docs/auditions/bass/<name>.wav for the
//                                     user's keep / re-voice / cut triage.
//
// The loudness math mirrors test_preset_loudness.cpp exactly (same 2.6 s middle-C
// hold, same windows, same classifier) so the numbers printed here ARE the gated
// numbers.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "PluginProcessor.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
    constexpr double kSR = 48000.0;

    double toDb (double rms) { return 20.0 * std::log10 (std::max (rms, 1.0e-9)); }

    double windowRmsDb (const std::vector<float>& mono, double t0, double t1)
    {
        const int a = (int) (kSR * t0), b = (int) (kSR * t1);
        double acc = 0.0; int cnt = 0;
        for (int i = a; i < b && i < (int) mono.size(); ++i) { acc += double (mono[(size_t) i]) * mono[(size_t) i]; ++cnt; }
        return toDb (cnt ? std::sqrt (acc / cnt) : 0.0);
    }

    struct Meas { double fullDb; double earlyDb; double lateDb; };

    Meas measure (VASynthProcessor& p)
    {
        p.prepareToPlay (kSR, 512);
        const int total = (int) (kSR * 2.6);
        std::vector<float> mono; mono.reserve ((size_t) total);
        int done = 0;
        while (done < total)
        {
            const int n = std::min (512, total - done);
            juce::AudioBuffer<float> buf (2, n); buf.clear();
            juce::MidiBuffer midi;
            if (done == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
            p.processBlock (buf, midi);
            const float* L = buf.getReadPointer (0); const float* R = buf.getReadPointer (1);
            for (int i = 0; i < n; ++i) mono.push_back (0.5f * (L[i] + R[i]));
            done += n;
        }
        return { windowRmsDb (mono, 0.05, 2.5), windowRmsDb (mono, 0.1, 0.5), windowRmsDb (mono, 1.5, 2.5) };
    }

    bool isSustainedClass (const juce::String& category, float ampS, const Meas& m)
    {
        if (category == "Drums" || category == "FX" || category == "Experimental") return false;
        if (ampS <= 0.25f) return false;
        return (m.lateDb - m.earlyDb) > -6.0;
    }
}

TEST_CASE ("bass rework: loudness report (median + per-patch delta)", "[.bassreport]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;

    struct Row { juce::String name, cat; float ampS; bool sust; double db; };
    std::vector<Row> rows;
    std::vector<double> sustainedDbs;

    for (auto& fp : p.factoryPresetLibrary().all())
    {
        p.loadFactoryPreset (fp.name);
        const float ampS = p.currentVoiceParams().ampS;
        const Meas m = measure (p);
        const bool s = isSustainedClass (fp.category, ampS, m);
        rows.push_back ({ fp.name, fp.category, ampS, s, m.fullDb });
        if (s) sustainedDbs.push_back (m.fullDb);
    }

    std::sort (sustainedDbs.begin(), sustainedDbs.end());
    const double median = sustainedDbs.empty() ? 0.0 : sustainedDbs[sustainedDbs.size() / 2];

    std::printf ("=== SUSTAINED BANK MEDIAN = %.2f dBFS  (n=%d) ===\n", median, (int) sustainedDbs.size());
    for (auto& r : rows)
    {
        if (r.cat != "Bass") continue;
        std::printf ("  BASS  %-16s  db=%7.2f  sust=%s  delta=%7.2f  ampS=%.2f  trim->median=%.3f\n",
                     r.name.toRawUTF8(), r.db, r.sust ? "Y" : "n",
                     r.sust ? (r.db - median) : 0.0,
                     r.ampS, std::pow (10.0, (median - r.db) / 20.0));
    }
    std::fflush (stdout);
    REQUIRE (true);
}

TEST_CASE ("bass rework: render an audition phrase per Bass patch to docs/auditions/bass", "[.audition]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;

    // A short, syncopated bass riff (MIDI note, velocity, on-time s, length s). One musical
    // phrase reused for every patch so keep/re-voice/cut is an apples-to-apples audition.
    struct Ev { int note; int vel; double on; double len; };
    const std::vector<Ev> phrase {
        { 40, 112, 0.00, 0.22 }, { 40,  84, 0.25, 0.18 }, { 52,  96, 0.50, 0.11 }, { 40, 104, 0.75, 0.22 },
        { 43, 112, 1.00, 0.22 }, { 40,  84, 1.25, 0.18 }, { 45, 104, 1.50, 0.22 }, { 40,  92, 1.75, 0.18 },
        { 38, 112, 2.00, 0.55 }, { 45,  96, 2.75, 0.18 }, { 43, 104, 3.00, 0.22 }, { 52,  96, 3.25, 0.11 },
        { 40, 112, 3.50, 0.60 }
    };
    const double phraseEnd = 4.6;   // last note + release tail
    const int total = (int) (kSR * phraseEnd);

    juce::File outDir (juce::String (VASYNTH_DOCS_DIR) + "/auditions/bass");
    outDir.createDirectory();

    auto sanitize = [] (const juce::String& n)
    {
        juce::String s;
        for (auto c : n) s += (juce::CharacterFunctions::isLetterOrDigit (c) ? juce::String::charToString (c)
                              : (c == ' ' ? juce::String ("_") : juce::String()));
        return s.toLowerCase();
    };

    int rendered = 0;
    for (auto& fp : p.factoryPresetLibrary().all())
    {
        if (fp.category != "Bass") continue;
        p.loadFactoryPreset (fp.name);
        p.prepareToPlay (kSR, 512);

        // Build the sample-accurate MIDI once, dispatched block by block.
        juce::AudioBuffer<float> render (2, total); render.clear();
        int done = 0;
        while (done < total)
        {
            const int n = std::min (512, total - done);
            juce::AudioBuffer<float> buf (2, n); buf.clear();
            juce::MidiBuffer midi;
            for (auto& e : phrase)
            {
                const int onS  = (int) (e.on * kSR);
                const int offS = (int) ((e.on + e.len) * kSR);
                if (onS  >= done && onS  < done + n) midi.addEvent (juce::MidiMessage::noteOn  (1, e.note, (juce::uint8) e.vel), onS  - done);
                if (offS >= done && offS < done + n) midi.addEvent (juce::MidiMessage::noteOff (1, e.note),                     offS - done);
            }
            p.processBlock (buf, midi);
            for (int ch = 0; ch < 2; ++ch) render.copyFrom (ch, done, buf, ch, 0, n);
            done += n;
        }

        auto f = outDir.getChildFile (sanitize (fp.name) + ".wav");
        f.deleteFile();
        juce::WavAudioFormat wav;
        if (auto os = f.createOutputStream())
        {
            std::unique_ptr<juce::AudioFormatWriter> w (
                wav.createWriterFor (os.release(), kSR, 2, 24, {}, 0));
            if (w != nullptr) { w->writeFromAudioSampleBuffer (render, 0, total); ++rendered; }
        }
    }

    juce::Logger::writeToLog ("=== rendered " + juce::String (rendered) + " bass auditions to " + outDir.getFullPathName() + " ===");
    REQUIRE (rendered >= 20);
}
