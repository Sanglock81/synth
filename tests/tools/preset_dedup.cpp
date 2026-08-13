// ============================================================================
// Render-similarity dedup tool (rerunnable; NOT an audition-WAV renderer).
//
// Renders EVERY factory preset offline through the real processor with ONE fixed
// short phrase, extracts a timbral fingerprint (average magnitude spectrum) plus
// an amplitude-envelope shape, computes a pairwise distance over the whole bank,
// and writes the TOP-30 most-similar pairs to docs/preset-similarity.md as a
// ranked table for a human pruning pass. No audio files are written.
//
//   preset_dedup [out.md]      # default: <docs>/preset-similarity.md
//
// Re-run any time (via scripts/preset-dedup.sh) after editing the bank.
// ============================================================================
#include "PluginProcessor.h"
#include <juce_events/juce_events.h>
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#ifndef VASYNTH_DOCS_DIR
 #define VASYNTH_DOCS_DIR "docs"
#endif

namespace
{
    constexpr double kSR      = 48000.0;
    constexpr int    kBlock   = 512;
    constexpr int    kFftOrder= 11;             // 2048-point FFT
    constexpr int    kFftSize = 1 << kFftOrder;
    constexpr int    kBins    = kFftSize / 2;
    constexpr double kRenderS = 2.2;            // phrase + tail
    constexpr int    kEnvHops = 64;             // amplitude-envelope resolution

    struct Fingerprint
    {
        juce::String name, category;
        std::vector<float> spectrum;            // normalized avg magnitude (unit sum)
        std::vector<float> envelope;            // peak-normalized RMS shape
    };

    // A fixed, deterministic short phrase: a rising triad then a low root, so the
    // fingerprint sees both bright and low registers and an attack + a sustain.
    // (note, onSample, offSample)
    struct Ev { int note; int on; int off; };
    std::vector<Ev> phrase()
    {
        const int s = (int) kSR;
        return {
            { 60, (int)(0.00 * s), (int)(0.55 * s) },
            { 64, (int)(0.25 * s), (int)(0.80 * s) },
            { 67, (int)(0.50 * s), (int)(1.05 * s) },
            { 72, (int)(0.75 * s), (int)(1.30 * s) },
            { 48, (int)(1.10 * s), (int)(1.70 * s) },
        };
    }

    Fingerprint render (VASynthProcessor& p, const juce::String& name, const juce::String& cat)
    {
        p.prepareToPlay (kSR, kBlock);
        const int total = (int) (kSR * kRenderS);
        std::vector<float> mono; mono.reserve ((size_t) total);

        auto evs = phrase();
        int done = 0;
        while (done < total)
        {
            const int n = std::min (kBlock, total - done);
            juce::AudioBuffer<float> buf (2, n); buf.clear();
            juce::MidiBuffer midi;
            for (auto& e : evs)
            {
                if (e.on  >= done && e.on  < done + n) midi.addEvent (juce::MidiMessage::noteOn  (1, e.note, 0.9f), e.on  - done);
                if (e.off >= done && e.off < done + n) midi.addEvent (juce::MidiMessage::noteOff (1, e.note),       e.off - done);
            }
            p.processBlock (buf, midi);
            const float* L = buf.getReadPointer (0); const float* R = buf.getReadPointer (1);
            for (int i = 0; i < n; ++i) mono.push_back (0.5f * (L[i] + R[i]));
            done += n;
        }

        Fingerprint fp; fp.name = name; fp.category = cat;

        // --- Amplitude envelope (peak-normalized RMS over kEnvHops windows) ------
        fp.envelope.assign ((size_t) kEnvHops, 0.0f);
        const int hop = std::max (1, total / kEnvHops);
        float envPeak = 1.0e-9f;
        for (int h = 0; h < kEnvHops; ++h)
        {
            double acc = 0.0; int cnt = 0;
            for (int i = h * hop; i < (h + 1) * hop && i < total; ++i) { acc += double (mono[(size_t) i]) * mono[(size_t) i]; ++cnt; }
            const float r = cnt ? (float) std::sqrt (acc / cnt) : 0.0f;
            fp.envelope[(size_t) h] = r;
            envPeak = std::max (envPeak, r);
        }
        for (auto& v : fp.envelope) v /= envPeak;   // shape only, level-independent

        // --- Average magnitude spectrum (Hann-windowed, overlapped) --------------
        juce::dsp::FFT fft (kFftOrder);
        std::vector<float> win ((size_t) kFftSize);
        for (int i = 0; i < kFftSize; ++i) win[(size_t) i] = 0.5f * (1.0f - std::cos (2.0f * juce::MathConstants<float>::pi * (float) i / (float) (kFftSize - 1)));

        fp.spectrum.assign ((size_t) kBins, 0.0f);
        std::vector<float> fftbuf ((size_t) (2 * kFftSize));
        const int step = kFftSize / 2;
        int frames = 0;
        for (int start = 0; start + kFftSize <= total; start += step)
        {
            std::fill (fftbuf.begin(), fftbuf.end(), 0.0f);
            for (int i = 0; i < kFftSize; ++i) fftbuf[(size_t) i] = mono[(size_t) (start + i)] * win[(size_t) i];
            fft.performFrequencyOnlyForwardTransform (fftbuf.data());
            for (int b = 0; b < kBins; ++b) fp.spectrum[(size_t) b] += fftbuf[(size_t) b];
            ++frames;
        }
        if (frames > 0) for (auto& v : fp.spectrum) v /= (float) frames;

        // Log-compress (perceptual) then normalize to unit sum -> a timbral profile
        // that ignores absolute loudness.
        double sum = 0.0;
        for (auto& v : fp.spectrum) { v = std::log1p (v); sum += v; }
        if (sum > 1.0e-9) for (auto& v : fp.spectrum) v = (float) (v / sum);

        return fp;
    }

    float specDist (const Fingerprint& a, const Fingerprint& b)   // L1 over normalized log-spectra (0..2)
    {
        double d = 0.0;
        for (size_t i = 0; i < a.spectrum.size(); ++i) d += std::abs (a.spectrum[i] - b.spectrum[i]);
        return (float) d;
    }
    float envDist (const Fingerprint& a, const Fingerprint& b)    // RMS diff of shape (0..~1)
    {
        double d = 0.0;
        for (size_t i = 0; i < a.envelope.size(); ++i) { const double e = a.envelope[i] - b.envelope[i]; d += e * e; }
        return (float) std::sqrt (d / (double) a.envelope.size());
    }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;

    const juce::String outPath = (argc > 1) ? juce::String (argv[1])
                                            : juce::String (VASYNTH_DOCS_DIR) + "/preset-similarity.md";

    std::vector<Fingerprint> fps;
    for (auto& fp : p.factoryPresetLibrary().all())
    {
        p.loadFactoryPreset (fp.name);
        fps.push_back (render (p, fp.name, fp.category));
        std::printf ("  fingerprinted %-28s [%s]\n", fp.name.toRawUTF8(), fp.category.toRawUTF8());
    }
    std::printf ("preset_dedup: %zu presets fingerprinted\n", fps.size());

    struct Pair { int a, b; float dist, sd, ed; };
    std::vector<Pair> pairs;
    pairs.reserve (fps.size() * fps.size() / 2);
    for (size_t i = 0; i < fps.size(); ++i)
        for (size_t j = i + 1; j < fps.size(); ++j)
        {
            const float sd = specDist (fps[i], fps[j]);
            const float ed = envDist  (fps[i], fps[j]);
            // Timbre dominates; envelope shape breaks ties. Both are loudness-independent.
            const float d = sd + 0.5f * ed;
            pairs.push_back ({ (int) i, (int) j, d, sd, ed });
        }

    std::sort (pairs.begin(), pairs.end(), [] (const Pair& x, const Pair& y) { return x.dist < y.dist; });

    juce::String md;
    md << "# Factory preset similarity (top 30 most-similar pairs)\n\n";
    md << "Auto-generated by `tests/tools/preset_dedup.cpp` (run `scripts/preset-dedup.sh`). "
          "Each preset is rendered offline through the real processor with one fixed 5-note phrase; "
          "the fingerprint is the loudness-independent average log-magnitude spectrum (timbre) plus a "
          "peak-normalized amplitude-envelope shape. Distance = spectral L1 + 0.5 x envelope RMS. "
          "Smaller = more alike. Use this to find redundant patches to prune or differentiate.\n\n";
    md << "| # | Distance | Preset A | Preset B | spec | env |\n";
    md << "|--:|--:|---|---|--:|--:|\n";
    const int topN = std::min<int> (30, (int) pairs.size());
    for (int k = 0; k < topN; ++k)
    {
        const auto& pr = pairs[(size_t) k];
        const auto& A = fps[(size_t) pr.a]; const auto& B = fps[(size_t) pr.b];
        md << "| " << (k + 1)
           << " | " << juce::String (pr.dist, 3)
           << " | " << A.name << " (" << A.category << ")"
           << " | " << B.name << " (" << B.category << ")"
           << " | " << juce::String (pr.sd, 3)
           << " | " << juce::String (pr.ed, 3) << " |\n";
    }
    md << "\n_" << (int) fps.size() << " presets, "
       << (int) pairs.size() << " pairs compared._\n";

    juce::File out (outPath);
    out.getParentDirectory().createDirectory();
    if (! out.replaceWithText (md)) { std::printf ("FAIL: could not write %s\n", outPath.toRawUTF8()); return 1; }
    std::printf ("preset_dedup: wrote %s\n", outPath.toRawUTF8());
    return 0;
}
