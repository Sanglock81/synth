// ============================================================================
// MIDI clock generator: 24-ppq ticks at sample-accurate offsets (low jitter — pedal loopers are
// unforgiving), plus Start/Stop edges and jump re-alignment. JUCE-free (DSP-only).
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "MidiClock.h"
#include <vector>
#include <cmath>
#include <algorithm>

namespace
{
    struct Ev { long pos; int kind; };

    // Run the generator for `blocks` blocks of `n` samples at `bpm`/`sr`, from beat 0.
    // Returns every emitted event with its ABSOLUTE sample position.
    std::vector<Ev> run (double bpm, double sr, int n, int blocks, bool running = true)
    {
        MidiClockGenerator gen; gen.reset();
        const double samplesPerBeat = sr * 60.0 / bpm;
        std::vector<Ev> out;
        double beats = 0.0;
        for (int b = 0; b < blocks; ++b)
        {
            const long base = (long) b * n;
            gen.process (beats, samplesPerBeat, n, /*enabled*/ true, running,
                         [&] (int off, int kind) { out.push_back ({ base + off, kind }); });
            beats += (double) n / samplesPerBeat;
        }
        return out;
    }

    std::vector<long> clockPositions (const std::vector<Ev>& evs)
    {
        std::vector<long> p;
        for (auto& e : evs) if (e.kind == MidiClockGenerator::Clock) p.push_back (e.pos);
        return p;
    }
}

TEST_CASE ("midi clock: 24 ppq ~1000 samples/tick @120 BPM/48k, jitter <= 1 sample", "[dsp][midiclock][jitter]")
{
    auto pos = clockPositions (run (120.0, 48000.0, 128, 1200));
    REQUIRE (pos.size() > 100);
    // 120 BPM @ 48k: a beat = 24000 samples, a 24-ppq tick = 1000 samples. Sample-accurate offsets
    // hold every gap to within 1 sample (~21 us) of the grid — far tighter than any pedal needs.
    long maxDev = 0;
    for (std::size_t i = 1; i < pos.size(); ++i) maxDev = std::max (maxDev, std::labs ((pos[i] - pos[i - 1]) - 1000));
    REQUIRE (maxDev <= 1);
}

TEST_CASE ("midi clock: fractional tempo stays within 1 sample of the ideal spacing", "[dsp][midiclock][jitter]")
{
    const double bpm = 137.0, sr = 48000.0;
    const double ideal = sr * 60.0 / bpm / MidiClockGenerator::kPpq;   // samples per tick
    auto pos = clockPositions (run (bpm, sr, 128, 1200));
    REQUIRE (pos.size() > 100);
    // Sample-accurate offsets: consecutive spacings round to the nearest sample, so every gap is
    // within 1 sample of the ideal and the long-run average matches it tightly (no drift).
    long minGap = 1 << 30, maxGap = 0;
    for (std::size_t i = 1; i < pos.size(); ++i)
    {
        const long g = pos[i] - pos[i - 1];
        minGap = std::min (minGap, g); maxGap = std::max (maxGap, g);
        REQUIRE (std::abs ((double) g - ideal) < 1.5);
    }
    REQUIRE ((maxGap - minGap) <= 1);                                  // total jitter <= 1 sample
    const double avg = (double) (pos.back() - pos.front()) / (double) (pos.size() - 1);
    REQUIRE (avg == Catch::Approx (ideal).margin (0.05));             // no long-term drift
}

TEST_CASE ("midi clock: block size does not change tick spacing", "[dsp][midiclock][jitter]")
{
    // Same tempo, different block sizes -> identical 24-ppq grid (spacing independent of blocks).
    for (int n : { 32, 64, 256, 512 })
    {
        auto pos = clockPositions (run (120.0, 48000.0, n, 64 * (512 / n)));
        for (std::size_t i = 1; i < pos.size(); ++i) REQUIRE (std::labs ((pos[i] - pos[i - 1]) - 1000) <= 1);
    }
}

TEST_CASE ("midi clock: Start on the running edge, Stop when it stops", "[dsp][midiclock]")
{
    MidiClockGenerator gen; gen.reset();
    const double spb = 48000.0 * 60.0 / 120.0;
    std::vector<int> kinds;
    auto emit = [&] (int, int k) { kinds.push_back (k); };
    double beats = 0.0;
    auto block = [&] (bool running) { gen.process (beats, spb, 128, true, running, emit); beats += 128.0 / spb; };

    kinds.clear(); block (true);    // first running block -> a Start
    REQUIRE (std::count (kinds.begin(), kinds.end(), (int) MidiClockGenerator::Start) == 1);
    kinds.clear(); block (true);    // steady -> only clocks, no more Starts
    REQUIRE (std::count (kinds.begin(), kinds.end(), (int) MidiClockGenerator::Start) == 0);
    kinds.clear(); block (false);   // stop -> a Stop
    REQUIRE (std::count (kinds.begin(), kinds.end(), (int) MidiClockGenerator::Stop) == 1);
}

TEST_CASE ("midi clock: a transport jump re-aligns instead of bursting", "[dsp][midiclock]")
{
    MidiClockGenerator gen; gen.reset();
    const double spb = 48000.0 * 60.0 / 120.0;
    int clocks = 0;
    auto emit = [&] (int, int k) { if (k == MidiClockGenerator::Clock) ++clocks; };
    // Normal block from beat 0, then a big forward jump (host locate to beat 64): the block must not
    // emit hundreds of catch-up ticks — at most a block's worth.
    gen.process (0.0, spb, 128, true, true, emit);
    clocks = 0;
    gen.process (64.0, spb, 128, true, true, emit);
    REQUIRE (clocks <= 4);          // ~ one block of ticks (128/1000 beat), not 64*24
}
