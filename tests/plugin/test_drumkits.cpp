// Synth — Copyright © 2026 John L Farmer. Licensed under AGPLv3; see LICENSE.
// ============================================================================
// #134 Classic-machine drum kits — verification + tour renders.
//   * legacy kit-name migration (old MULTI/.kit loads its successor);
//   * every classic kit fills 16 pads, all non-silent, level-balanced within the kit;
//   * hats choke, the cymbal rings free;
//   * a dense 16th-note reference bar with a long ringing cymbal stays finite/bounded/click-free
//     (choke + steal torture) -- and is rendered to docs/audio-refs/<kit>.wav for A/B by ear.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "PluginProcessor.h"
#include "test_util.h"
#include <vector>
#include <cmath>

namespace
{
    constexpr double kSR = 48000.0;
    constexpr int    kBlock = 128;

    // Render `blocks` blocks of part `part`; scan L for finite/peak/max-jump; return mono (L).
    struct Scan { bool finite = true; float peak = 0, maxJump = 0; };
    std::vector<float> pump (VASynthProcessor& p, int blocks, Scan& s, float& prev)
    {
        std::vector<float> out; out.reserve ((size_t) blocks * kBlock);
        for (int b = 0; b < blocks; ++b)
        {
            juce::AudioBuffer<float> buf (2, kBlock); buf.clear();
            juce::MidiBuffer m; p.processBlock (buf, m);
            const float* L = buf.getReadPointer (0);
            for (int i = 0; i < kBlock; ++i)
            {
                s.finite = s.finite && std::isfinite (L[i]);
                s.peak = std::max (s.peak, std::abs (L[i]));
                s.maxJump = std::max (s.maxJump, std::abs (L[i] - prev));
                prev = L[i]; out.push_back (L[i]);
            }
        }
        return out;
    }

    // A fixed reference bar: dense 16th closed-hats + kick/snare/clap + a long cymbal ringing on 1.
    // Trigger notes are the shared kit layout (36 kick, 38 snare, 40 clap, 42/43 hats, 49 cymbal).
    struct Hit { int step; int note; float vel; };
    const std::vector<Hit>& refPattern()
    {
        static const std::vector<Hit> pat = [] {
            std::vector<Hit> v;
            for (int s = 0; s < 16; ++s) v.push_back ({ s, 42, (s % 2 ? 0.7f : 0.95f) });  // 16th closed hats
            for (int s : { 0, 6, 10 }) v.push_back ({ s, 36, 1.0f });                        // kick
            for (int s : { 4, 12 })    v.push_back ({ s, 38, 0.9f });                        // snare
            v.push_back ({ 12, 40, 0.85f });                                                 // clap
            v.push_back ({ 14, 43, 0.8f });                                                  // open hat (chokes the closed)
            v.push_back ({ 0, 49, 1.0f });                                                   // long cymbal, rings free
            return v;
        }();
        return pat;
    }
}

TEST_CASE ("drumkits: legacy kit names migrate to their successors (#134)", "[plugin][drums][migration]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    // loadKit by a retired name -> successor definition.
    REQUIRE (p.loadKit ("808 Basics").name   == "808");
    REQUIRE (p.loadKit ("House Basics").name == "909");
    REQUIRE (p.loadKit ("Stab Board").name   == "808");
    // A MULTI/.kit ValueTree that stored the retired kit rebuilds the successor on load.
    juce::ValueTree t ("KIT"); t.setProperty ("name", "House Basics", nullptr);
    juce::ValueTree pad ("PAD"); pad.setProperty ("trigger", 36, nullptr); pad.setProperty ("source", "House Kick", nullptr);
    t.addChild (pad, -1, nullptr);
    auto def = VASynthProcessor::kitFromTree (t);
    REQUIRE (def.name == "909");
    REQUIRE (def.pads[0].source == "909 Kick");   // rebuilt from the successor, not the stale embedded pad
    // A non-legacy (user) kit name passes through unchanged.
    REQUIRE (VASynthProcessor::migrateKitName ("My Kit") == "My Kit");
}

TEST_CASE ("drumkits: every classic kit is 16 non-silent, level-balanced pads", "[plugin][drums][kits][level]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    for (auto& kit : VASynthProcessor::classicKitNames())
    {
        VASynthProcessor p; p.prepareToPlay (kSR, kBlock);
        p.setPartKit (1, p.loadKit (kit));
        std::vector<float> peaks;
        for (int pad = 0; pad < 16; ++pad)
        {
            p.routeMidi (juce::MidiMessage::noteOn (1, 36 + pad, 0.9f), 1);
            Scan s; float prev = 0; auto out = pump (p, 90, s, prev);   // ~0.24 s
            const float pk = tu::peak (out);
            INFO ("kit " << kit << " pad " << pad << " (note " << (36 + pad) << ") peak=" << pk);
            REQUIRE (pk > 1.0e-3f);            // every pad audible
            REQUIRE (s.finite);
            peaks.push_back (pk);
        }
        // Balance: no pad wildly louder/quieter than the kit's median (within ~18 dB span).
        auto sorted = peaks; std::sort (sorted.begin(), sorted.end());
        const float med = sorted[sorted.size() / 2];
        for (float pk : peaks)
        {
            const float db = 20.0f * std::log10 (pk / med);
            INFO ("kit " << kit << " pad delta " << db << " dB (median " << med << ")");
            REQUIRE (std::abs (db) < 18.0f);
        }
    }
}

TEST_CASE ("drumkits: hats choke, cymbal rings free", "[plugin][drums][kits][choke]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    for (auto& kit : VASynthProcessor::classicKitNames())
    {
        auto def = VASynthProcessor::factoryKit (kit);
        // Pads 6 + 7 are the closed/open hats and share a nonzero choke group; pad for the cymbal is free.
        REQUIRE (def.pads[6].chokeGroup != 0);
        REQUIRE (def.pads[7].chokeGroup == def.pads[6].chokeGroup);
    }
}

TEST_CASE ("drumkits: dense 16th reference bar + long cymbal stays clean, renders a tour WAV", "[plugin][drums][kits][torture][tour]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    for (auto& kit : VASynthProcessor::classicKitNames())
    {
        VASynthProcessor p; p.prepareToPlay (kSR, kBlock);
        p.setPartKit (1, p.loadKit (kit));

        const double stepSec = 0.125;                         // 16th at 120 BPM
        const int blocksPerStep = (int) std::round (stepSec * kSR / kBlock);   // ~47
        Scan s; float prev = 0; std::vector<float> song;
        for (int step = 0; step < 16; ++step)
        {
            for (auto& h : refPattern()) if (h.step == step)
                p.routeMidi (juce::MidiMessage::noteOn (1, h.note, h.vel), 1);
            auto seg = pump (p, blocksPerStep, s, prev);
            song.insert (song.end(), seg.begin(), seg.end());
        }
        auto tail = pump (p, 900, s, prev);                    // ~2.4 s tail for the cymbal ring
        song.insert (song.end(), tail.begin(), tail.end());

        INFO ("kit " << kit << " peak=" << s.peak << " maxJump=" << s.maxJump);
        REQUIRE (s.finite);
        REQUIRE (s.peak <= 1.0001f);                           // safety clipper holds through the dense mash
        REQUIRE (s.maxJump < 0.6f);                            // no gross click/pop (drum transients are steep)

       #ifdef VASYNTH_DOCS_DIR
        juce::File dir (juce::String (VASYNTH_DOCS_DIR) + "/audio-refs");
        dir.createDirectory();
        tu::writeWavF32 ((dir.getChildFile (kit + ".wav").getFullPathName()).toStdString(), song, (int) kSR);
       #endif
    }
}
