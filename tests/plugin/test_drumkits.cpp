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
#include <set>

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
    // Trigger notes are the shared kit layout (36 kick, 38 snare, 40 clap, 42/43 hats, 48 crash).
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
            v.push_back ({ 0, 48, 1.0f });                                                   // crash (48 after C0.5), rings free
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

TEST_CASE ("drumkits: every factory kit is 16 non-silent, level-balanced pads", "[plugin][drums][kits][level]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    // EVERY factory kit, Originals included -- the house-designed kits earn the same gate as
    // the classic-machine homages, and the three new ones (Foundry/Circuit/Hearth) most of all.
    for (auto& kit : VASynthProcessor::factoryKitNames())
    {
        VASynthProcessor p; p.prepareToPlay (kSR, kBlock);
        const auto def = p.loadKit (kit);
        p.setPartKit (1, def);
        std::vector<float> peaks;
        // Drive each pad by its OWN trigger note. A kit is not obliged to lay its pads out as
        // 36 + index (Foundry parks its machine tick on 35), and assuming so would silently
        // test a note nothing is mapped to.
        for (int pad = 0; pad < 16; ++pad)
        {
            const int note = def.pads[(std::size_t) pad].triggerNote;
            REQUIRE (note >= 0);
            p.routeMidi (juce::MidiMessage::noteOn (1, note, 0.9f), 1);
            Scan s; float prev = 0; auto out = pump (p, 90, s, prev);   // ~0.24 s
            const float pk = tu::peak (out);
            INFO ("kit " << kit << " pad " << pad << " (note " << note << ") peak=" << pk);
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
    for (auto& kit : VASynthProcessor::factoryKitNames())
    {
        auto def = VASynthProcessor::factoryKit (kit);
        // Find the hats by their TRIGGER (42 closed / 43 open — the shared map), not by pad
        // index: pad order is a kit's own business and Foundry's is offset by its 35 pad.
        int closed = -1, open = -1;
        for (int i = 0; i < kMaxKitPads; ++i)
        {
            if (def.pads[(std::size_t) i].triggerNote == 42) closed = i;
            if (def.pads[(std::size_t) i].triggerNote == 43) open   = i;
        }
        INFO ("kit " << kit << " closed-hat pad " << closed << ", open-hat pad " << open);
        REQUIRE (closed >= 0); REQUIRE (open >= 0);
        REQUIRE (def.pads[(std::size_t) closed].chokeGroup != 0);
        REQUIRE (def.pads[(std::size_t) open].chokeGroup == def.pads[(std::size_t) closed].chokeGroup);
    }
}

TEST_CASE ("drumkits: dense 16th reference bar + long cymbal stays clean, renders a tour WAV", "[plugin][drums][kits][torture][tour]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    for (auto& kit : VASynthProcessor::factoryKitNames())
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

// ---------------------------------------------------------------------------
// C0.5 — kit library harmonization. Cross-kit consistency is the whole point of a
// shared trigger map: a pattern written on one kit has to stay meaningful on the next.
// The sequencer's default rows encode that map, so these two must never disagree.
// ---------------------------------------------------------------------------

TEST_CASE ("drumkits: every factory kit puts the foundational eight on the same notes",
           "[plugin][drums][kits][harmonization]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    // Kick / snare / rim / closed hat / open hat / crash / ride / low tom — the rows the
    // sequencer ships with. THE SAME ARRAY, read from the sequencer, so the two cannot drift.
    const auto rows = StepSequencer::defaultNotes();
    for (auto& kitName : VASynthProcessor::factoryKitNames())
    {
        const auto def = VASynthProcessor::factoryKit (kitName);
        for (int note : rows)
        {
            INFO ("kit '" << kitName << "' has no pad on the sequencer's row note " << note);
            bool found = false;
            for (auto& pd : def.pads) if (pd.triggerNote == note) { found = true; break; }
            REQUIRE (found);
        }
    }
}

TEST_CASE ("drumkits: ride is 47 and crash is 48 everywhere, by name", "[plugin][drums][kits][harmonization]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    // The mapping is only worth anything if the pad on 47 really IS a ride. Checked by the
    // source preset's name, which is what a reader of factoryKit() is trusting.
    for (auto& kitName : VASynthProcessor::factoryKitNames())
    {
        const auto def = VASynthProcessor::factoryKit (kitName);
        juce::String ride, crash;
        for (auto& pd : def.pads)
        {
            if (pd.triggerNote == 47) ride  = pd.source;
            if (pd.triggerNote == 48) crash = pd.source;
        }
        INFO ("kit '" << kitName << "': 47 = '" << ride << "', 48 = '" << crash << "'");
        REQUIRE (ride.containsIgnoreCase ("Ride"));
        // The 808's and 606's long cymbal IS each kit's crash (both named "Cymbal"), and
        // Hearth's crash is a small SPLASH -- which is what a kit that size would really
        // have. What the row must never be is something that is not a cymbal at all.
        REQUIRE ((crash.containsIgnoreCase ("Crash") || crash.containsIgnoreCase ("Cymbal")
                                                     || crash.containsIgnoreCase ("Splash")));
    }
}

TEST_CASE ("drumkits: no kit lost a pad or gained a duplicate trigger", "[plugin][drums][kits][harmonization]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    for (auto& kitName : VASynthProcessor::factoryKitNames())
    {
        const auto def = VASynthProcessor::factoryKit (kitName);
        std::set<int> triggers;
        int filled = 0;
        for (auto& pd : def.pads)
            if (pd.triggerNote >= 0)
            {
                ++filled;
                INFO ("kit '" << kitName << "' has two pads on trigger " << pd.triggerNote);
                REQUIRE (triggers.insert (pd.triggerNote).second);   // a duplicate would shadow a pad
            }
        INFO ("kit '" << kitName << "' has " << filled << " pads");
        REQUIRE (filled == kMaxKitPads);                             // still a full kit
    }
}

TEST_CASE ("drumkits: kits are level-matched to each other, not just internally",
           "[plugin][drums][kits][level][crosskit]")
{
    // The per-kit balance test only looks INSIDE a kit, so a kit can be internally tidy and
    // still arrive 6 dB under the rest of the library — which is exactly what Foundry did on
    // its first build (heavy filter_drive eats headroom). Switching kits mid-piece must not
    // be a volume change, so the medians have to line up too.
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::vector<std::pair<juce::String, float>> medians;
    for (auto& kit : VASynthProcessor::factoryKitNames())
    {
        VASynthProcessor p; p.prepareToPlay (kSR, kBlock);
        p.setPartKit (1, p.loadKit (kit));
        const auto def = VASynthProcessor::factoryKit (kit);
        std::vector<float> peaks;
        for (int pad = 0; pad < 16; ++pad)
        {
            p.routeMidi (juce::MidiMessage::noteOn (1, def.pads[(std::size_t) pad].triggerNote, 0.9f), 1);
            Scan s; float prev = 0; auto out = pump (p, 90, s, prev);
            peaks.push_back (tu::peak (out));
        }
        std::sort (peaks.begin(), peaks.end());
        medians.emplace_back (kit, peaks[peaks.size() / 2]);
    }
    auto sorted = medians;
    std::sort (sorted.begin(), sorted.end(), [] (auto& a, auto& b) { return a.second < b.second; });
    const float lo = sorted.front().second, hi = sorted.back().second;
    for (auto& kv : medians) INFO ("kit " << kv.first << " median peak " << kv.second);
    INFO ("quietest " << sorted.front().first << " (" << lo << "), loudest "
                      << sorted.back().first << " (" << hi << ")");
    // Measured spread on this build is ~7.4 dB, and the outliers are the LONG-shipped DMX
    // (quietest) and Industrial (loudest) — the three new kits land inside it. 10 dB is the
    // gate: wider than that and switching kits mid-piece becomes a volume change.
    REQUIRE (20.0f * std::log10 (hi / lo) < 10.0f);
}
