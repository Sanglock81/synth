// ============================================================================
// #85 MIDI clock OUT (processor level). The synth transmits 24-ppq clock + start/stop on its MIDI
// output: standalone follows the internal Tempo knob; in a DAW it relays the host tempo + play
// state. The instrument's MIDI OUT carries ONLY the clock (never echoes input notes). Tick spacing
// is measured for jitter across the real processBlock.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginProcessor.h"
#include <vector>

namespace
{
    void setVal (VASynthProcessor& p, const char* id, float v)
    { auto* pr = p.apvts.getParameter (id); pr->setValueNotifyingHost (pr->convertTo0to1 (v)); }

    struct MockHost : juce::AudioPlayHead
    {
        double bpm = 120.0, ppq = 0.0; bool playing = true;
        juce::Optional<PositionInfo> getPosition() const override
        { PositionInfo pi; pi.setBpm (bpm); pi.setPpqPosition (ppq); pi.setIsPlaying (playing); return pi; }
    };

    // Drive `blocks` blocks of `n` samples; collect (absolutePos, statusByte) for every clock/start/
    // stop the processor emits into its output MIDI buffer. If `host`, advance its ppq per block.
    struct Cap { long pos; int status; };
    std::vector<Cap> pump (VASynthProcessor& p, int blocks, int n, MockHost* host = nullptr)
    {
        juce::AudioBuffer<float> buf (2, n);
        std::vector<Cap> out;
        const double bpm = host ? host->bpm : 120.0;
        const double beatsPerBlock = (bpm / 60.0) * ((double) n / 48000.0);
        for (int b = 0; b < blocks; ++b)
        {
            buf.clear(); juce::MidiBuffer m;
            p.processBlock (buf, m);
            for (const auto meta : m)
            {
                const auto msg = meta.getMessage();
                if (msg.isMidiClock())      out.push_back ({ (long) b * n + meta.samplePosition, 0xF8 });
                else if (msg.isMidiStart()) out.push_back ({ (long) b * n + meta.samplePosition, 0xFA });
                else if (msg.isMidiStop())  out.push_back ({ (long) b * n + meta.samplePosition, 0xFC });
            }
            if (host) host->ppq += beatsPerBlock;
        }
        return out;
    }
    std::vector<long> clocks (const std::vector<Cap>& c)
    { std::vector<long> p; for (auto& e : c) if (e.status == 0xF8) p.push_back (e.pos); return p; }
    int count (const std::vector<Cap>& c, int status)
    { int n = 0; for (auto& e : c) if (e.status == status) ++n; return n; }
}

TEST_CASE ("clock out: disabled by default -> no MIDI emitted", "[plugin][clockout]")
{
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    auto ev = pump (p, 200, 128);
    REQUIRE (ev.empty());
}

TEST_CASE ("clock out (standalone master): 24 ppq at the internal tempo, jitter <= 1 sample", "[plugin][clockout][jitter]")
{
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    setVal (p, ParamID::tempo, 120.0f);
    setVal (p, ParamID::clockOut, 1.0f);
    auto ev = pump (p, 1200, 128);              // no playhead => standalone, always running

    REQUIRE (count (ev, 0xFA) == 1);            // one Start at the top
    auto pos = clocks (ev);
    REQUIRE (pos.size() > 100);
    long maxDev = 0;
    for (std::size_t i = 1; i < pos.size(); ++i) maxDev = std::max (maxDev, std::labs ((pos[i] - pos[i - 1]) - 1000));
    REQUIRE (maxDev <= 1);                       // 120 BPM/48k -> 1000 samples/tick, within a sample
    // ONLY clock: a held note must not be echoed to the MIDI output.
    p.routeNoteOn (60, 0.9f, 0);
    auto ev2 = pump (p, 20, 128);
    REQUIRE (count (ev2, 0xF8) > 0);
    for (auto& e : ev2) REQUIRE ((e.status == 0xF8 || e.status == 0xFA || e.status == 0xFC));
}

TEST_CASE ("clock out (DAW relay): follows host tempo + start/stop", "[plugin][clockout]")
{
    VASynthProcessor p; p.prepareToPlay (48000.0, 128);
    setVal (p, ParamID::clockOut, 1.0f);
    MockHost host; host.bpm = 150.0; host.playing = true; p.setPlayHead (&host);

    auto ev = pump (p, 800, 128, &host);
    REQUIRE (count (ev, 0xFA) >= 1);            // Start when the host is playing
    auto pos = clocks (ev);
    REQUIRE (pos.size() > 60);
    const double ideal = 48000.0 * 60.0 / host.bpm / 24.0;   // samples/tick @150
    for (std::size_t i = 1; i < pos.size(); ++i) REQUIRE (std::abs ((double) (pos[i] - pos[i - 1]) - ideal) < 1.5);

    // Host stops -> a Stop message; clock ticks cease.
    host.playing = false;
    auto ev2 = pump (p, 200, 128, &host);
    REQUIRE (count (ev2, 0xFC) == 1);
    REQUIRE (count (ev2, 0xF8) == 0);
    p.setPlayHead (nullptr);
}
