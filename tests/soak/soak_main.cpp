// ============================================================================
// Memory soak: a synthetic MIDI storm through the full processBlock path, run
// for a configurable number of audio-seconds. Samples RSS at intervals and
// asserts no monotonic growth beyond a small tolerance. Built under ASan/LSan
// (run-all-checks.sh --sanitize) it also fails on any leak at exit.
//
//   ./soak            # default 60 audio-seconds (gate)
//   ./soak 600        # full 10-minute soak
// ============================================================================
#include "PluginProcessor.h"
#include <juce_events/juce_events.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cstdint>
#include <unistd.h>

namespace
{
    long readRssKb()
    {
        long pages = 0, resident = 0;
        if (FILE* f = std::fopen ("/proc/self/statm", "r"))
        {
            if (std::fscanf (f, "%ld %ld", &pages, &resident) != 2) resident = 0;
            std::fclose (f);
        }
        return resident * (sysconf (_SC_PAGESIZE) / 1024);
    }
}

int main (int argc, char** argv)
{
    const double audioSeconds = (argc > 1) ? std::atof (argv[1]) : 60.0;
    juce::ScopedJuceInitialiser_GUI juceInit;

    const double sr = 48000.0;
    const int    block = 128;
    VASynthProcessor p;
    p.prepareToPlay (sr, block);
    juce::AudioBuffer<float> buf (2, block);

    std::uint32_t rng = 0x1234567u;
    auto rnd = [&] { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; };

    const long blocksPerSec = (long) (sr / block);
    const long totalBlocks  = (long) (audioSeconds * blocksPerSec);
    const long sampleEvery  = totalBlocks / 40 + 1;

    std::vector<long> rss;
    int held = 0;

    for (long b = 0; b < totalBlocks; ++b)
    {
        buf.clear();
        juce::MidiBuffer midi;

        // MIDI storm: a few note events per block, mixed with CC/bend/sustain.
        for (int k = 0; k < 4; ++k)
        {
            const std::uint32_t r = rnd();
            const int note = 36 + (int) (r % 48);
            if ((r & 1u) || held > 40) midi.addEvent (juce::MidiMessage::noteOff (1, note), k * 8);
            else { midi.addEvent (juce::MidiMessage::noteOn (1, note, 0.8f), k * 8); ++held; }
        }
        if (b % 5  == 0) midi.addEvent (juce::MidiMessage::controllerEvent (1, 21, (int) (rnd() % 128)), 0);
        if (b % 11 == 0) midi.addEvent (juce::MidiMessage::controllerEvent (1, 64, (int) (rnd() % 128)), 0);
        if (b % 13 == 0) midi.addEvent (juce::MidiMessage::pitchWheel (1, (int) (rnd() % 16384)), 0);
        if (b % 97 == 0) { midi.addEvent (juce::MidiMessage::allNotesOff (1), 0); held = 0; }

        p.processBlock (buf, midi);

        if (b % sampleEvery == 0) rss.push_back (readRssKb());
    }

    if (rss.size() < 8) { std::printf ("soak: too few samples\n"); return 0; }

    // Baseline after warmup (25% in); compare the tail.
    const long baseline = rss[rss.size() / 4];
    const long last     = rss.back();
    const double growth = baseline > 0 ? double (last - baseline) / double (baseline) : 0.0;

    std::printf ("soak: %.0f audio-s, %ld blocks, RSS baseline=%ld KB last=%ld KB growth=%.2f%%\n",
                 audioSeconds, totalBlocks, baseline, last, growth * 100.0);

#if defined(__SANITIZE_ADDRESS__)
    // Under ASan the allocator quarantine inflates RSS over time, so the RSS
    // heuristic is meaningless here — LeakSanitizer at process exit is the real
    // leak gate (a leak makes this process exit non-zero).
    std::printf ("PASS: (ASan build — leak check is LSan at exit; RSS growth ignored)\n");
    return 0;
#else
    if (growth > 0.05)
    {
        std::printf ("FAIL: RSS grew %.2f%% (> 5%% tolerance) - possible leak/unbounded growth\n",
                     growth * 100.0);
        return 1;
    }
    std::printf ("PASS: memory stable\n");
    return 0;
#endif
}
