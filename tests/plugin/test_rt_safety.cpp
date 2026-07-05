// ============================================================================
// Processor-layer real-time safety: VASynthProcessor::processBlock must not
// allocate on the audio thread. Covers the monoScratch preallocation fix.
//
// The MIDI-learn CC path is exercised separately once it is lock-free/alloc-free
// (see the [midilearn][rt] case, which sends CCs through processBlock).
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "PluginProcessor.h"
#include "alloc_hook.h"

namespace
{
    std::size_t allocsDuring (VASynthProcessor& p, int blocks, bool withCCs)
    {
        juce::AudioBuffer<float> buf (2, 512);
        std::size_t news = 0;
        {
            alloc_hook::AllocGuard g;
            for (int b = 0; b < blocks; ++b)
            {
                buf.clear();
                juce::MidiBuffer midi;
                if (withCCs)
                    midi.addEvent (juce::MidiMessage::controllerEvent (1, 21, b & 127), 0);
                p.processBlock (buf, midi);
            }
            news = g.count();
        }
        return news;
    }
}

TEST_CASE ("processBlock does not allocate while rendering voices", "[plugin][rt][alloc]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 512);

    // Activate a couple of voices and warm up any one-time JUCE lazy init.
    juce::AudioBuffer<float> buf (2, 512);
    {
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
        midi.addEvent (juce::MidiMessage::noteOn (1, 64, 0.7f), 8);
        buf.clear();
        p.processBlock (buf, midi);
    }
    for (int i = 0; i < 20; ++i) { buf.clear(); juce::MidiBuffer m; p.processBlock (buf, m); }

    // This also covers the observability logging path: processBlock now pushes
    // render-time / voice-count / steal / overrun events into the RT-safe ring
    // every block. Zero allocations here proves that logging stays RT-safe (the
    // drain thread's allocations happen on another thread and aren't counted).
    const std::size_t news = allocsDuring (p, 200, /*withCCs=*/false);
    INFO ("allocations in 200 note-rendering blocks = " << news);
    REQUIRE (news == 0);
}

TEST_CASE ("processBlock does not allocate while handling mapped CCs", "[plugin][rt][alloc][midilearn]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 512);

    juce::AudioBuffer<float> buf (2, 512);
    for (int i = 0; i < 20; ++i) { buf.clear(); juce::MidiBuffer m; p.processBlock (buf, m); }

    const std::size_t news = allocsDuring (p, 200, /*withCCs=*/true);
    INFO ("allocations in 200 CC-handling blocks = " << news);
    REQUIRE (news == 0);
}
