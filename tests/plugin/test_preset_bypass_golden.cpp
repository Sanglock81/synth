// Synth — Copyright © 2026 John L Farmer. Licensed under AGPLv3; see LICENSE.
// ============================================================================
// FACTORY-BANK RENDER GOLDEN — the shipped-sound regression gate.
//
// The existing golden covers ONE engine-level render. This covers the thing users
// actually own: a spread of factory PRESETS, taken end to end through the real
// processor (preset load -> APVTS -> voices -> FX -> master), hashed sample-exact,
// and compared against a committed table.
//
// It was created as the proof obligation for the NOISE XY field (Phase B), whose
// whole design rests on one promise: a patch that never touches the field takes a
// code path with no filter in it, so it renders the SAME SAMPLES as before the
// feature existed. The reference table below was generated from the tree WITHOUT
// the feature and must keep matching with it in. It stays as a permanent gate: any
// future change that moves a shipped preset's sound has to say so out loud by
// regenerating this file, rather than sliding through unnoticed.
//
// Deliberately NOT tolerance-based. "Close enough" is how sound drifts.
//
// TOOLCHAIN-SCOPED, on purpose. Sample-exactness is a property of ONE build: the
// engine leans on std::tan / std::pow / std::exp, whose last bits legitimately differ
// between platforms and libm versions, so a hash captured with GCC on Linux cannot
// match MSVC on Windows and it would be dishonest to pretend otherwise. The file
// records the toolchain that produced it and the exact comparison runs only there —
// on the reference/live target, which is where "this patch sounds exactly as it did"
// is the claim that matters. Elsewhere the render still has to happen, be finite and
// make sound; cross-platform sound agreement is the existing tolerance-based goldens'
// job (tests/dsp/test_golden.cpp), not this one's.
//
// Regenerating (only when a sound change is INTENDED and reviewed): delete
// tests/golden/preset_render_hashes.txt, run this test once, commit the new file.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "PluginProcessor.h"
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#ifndef VASYNTH_PRESET_GOLDEN_DIR
 #define VASYNTH_PRESET_GOLDEN_DIR "."
#endif

namespace
{
    constexpr double kSR    = 48000.0;
    constexpr int    kBlock = 256;

    // A spread across the bank rather than the first N: every category the library
    // groups by, both quiet and loud material, and the noise-carrying patches (the
    // ones the field would disturb first if the bypass were not a real code path).
    const std::vector<juce::String>& probePresets()
    {
        static const std::vector<juce::String> names {
            "Init",
            "Bright Lead", "Supersaw", "Theremin",
            "Acid Bass", "Hoover Bass", "Bite Sub",
            "Warm Pad", "Glass Pad", "Snowfield",
            "E-Piano", "Digital Bell", "Bell Keys",
            "Kalimba", "Harp Gliss",
            "Analog Brass", "Brass Fanfare",
            "String Machine", "Soft Flute", "Ocarina",
            "Cathedral Pipe", "Full Organ",
            // Noise-carrying and noise-adjacent material: if the field's bypass were ever
            // less than a true code path, these are where it would show up first.
            "Thunder Sheet", "Static Garden", "Dark Drone", "Sheet Storm",
            "Snare Studio", "Hat Closed Soft", "Crash Dark", "Shaker",
            "808 Kick", "909 Snare", "808 Hat Cl", "909 Crash", "606 Kick",
        };
        return names;
    }

    // FNV-1a over the raw float bits. Sample-exact by construction: one bit different
    // anywhere in the render moves the hash.
    std::uint64_t hashRender (const std::vector<float>& x)
    {
        std::uint64_t h = 1469598103934665603ull;
        for (float s : x)
        {
            std::uint32_t bits; std::memcpy (&bits, &s, sizeof bits);
            for (int b = 0; b < 4; ++b)
            { h ^= (std::uint64_t) ((bits >> (8 * b)) & 0xffu); h *= 1099511628211ull; }
        }
        return h;
    }

    // A fixed phrase: a chord held, then released, rendered to a fixed length. Stereo
    // interleaved so a change to either channel counts.
    std::vector<float> renderPreset (VASynthProcessor& p, const juce::String& name)
    {
        if (name == "Init") p.loadInitPreset();
        else                p.loadFactoryPreset (name);
        p.prepareToPlay (kSR, kBlock);

        const int blocks = 280;                        // ~1.5 s
        std::vector<float> out;
        out.reserve ((std::size_t) blocks * kBlock * 2);
        for (int b = 0; b < blocks; ++b)
        {
            juce::AudioBuffer<float> buf (2, kBlock);
            buf.clear();
            juce::MidiBuffer midi;
            if (b == 0)
            {
                midi.addEvent (juce::MidiMessage::noteOn (1, 48, 0.85f), 0);
                midi.addEvent (juce::MidiMessage::noteOn (1, 55, 0.70f), 64);
                midi.addEvent (juce::MidiMessage::noteOn (1, 64, 0.95f), 128);
            }
            if (b == 160)
            {
                midi.addEvent (juce::MidiMessage::noteOff (1, 48), 0);
                midi.addEvent (juce::MidiMessage::noteOff (1, 55), 0);
                midi.addEvent (juce::MidiMessage::noteOff (1, 64), 0);
            }
            p.processBlock (buf, midi);
            for (int i = 0; i < kBlock; ++i)
            { out.push_back (buf.getSample (0, i)); out.push_back (buf.getSample (1, i)); }
        }
        return out;
    }

    std::string goldenPath()
    { return std::string (VASYNTH_PRESET_GOLDEN_DIR) + "/preset_render_hashes.txt"; }

    // Which build produced (or can verify) a hash table. Anything that can move the last
    // bit of a transcendental belongs here — OS and compiler family are what actually vary
    // across our CI matrix.
    std::string toolchainTag()
    {
       #if defined (_WIN32)
        const char* os = "windows";
       #elif defined (__APPLE__)
        const char* os = "macos";
       #else
        const char* os = "linux";
       #endif
       #if defined (__clang__)
        const char* cc = "clang";
       #elif defined (_MSC_VER)
        const char* cc = "msvc";
       #elif defined (__GNUC__)
        const char* cc = "gcc";
       #else
        const char* cc = "unknown";
       #endif
        return std::string (os) + "-" + cc;
    }

    struct Golden { std::string tag; std::map<std::string, std::uint64_t> hashes; };

    Golden readGolden()
    {
        Golden g;
        if (FILE* f = std::fopen (goldenPath().c_str(), "r"))
        {
            char line[512];
            while (std::fgets (line, sizeof line, f) != nullptr)
            {
                std::string s (line);
                while (! s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
                if (s.empty()) continue;
                if (s.rfind ("#toolchain ", 0) == 0) { g.tag = s.substr (11); continue; }
                if (s[0] == '#') continue;
                const auto tab = s.find ('\t');
                if (tab == std::string::npos) continue;
                g.hashes[s.substr (0, tab)] = std::strtoull (s.c_str() + tab + 1, nullptr, 10);
            }
            std::fclose (f);
        }
        return g;
    }
}

TEST_CASE ("factory preset renders match the committed reference, sample for sample", "[plugin][golden][presets][noise]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;

    const auto ref = readGolden();
    std::map<std::string, std::uint64_t> got;
    for (auto& name : probePresets())
    {
        INFO ("preset: " << name);
        // A typo'd name would silently render Init and "pass" forever; refuse that.
        REQUIRE ((name == "Init" || p.factoryPresetLibrary().byName (name) != nullptr));
        const auto audio = renderPreset (p, name);
        // Everywhere, on every toolchain: the preset really rendered. Without this the
        // non-matching platforms would "pass" this test while doing nothing at all.
        bool finite = true; float peak = 0.0f;
        for (float s : audio) { finite = finite && std::isfinite (s); peak = std::max (peak, std::abs (s)); }
        REQUIRE (finite);
        REQUIRE (peak > 0.001f);
        got[name.toStdString()] = hashRender (audio);
    }
    REQUIRE (got.size() >= 20);                       // the bank spread this gate promises

    if (ref.hashes.empty())
    {
        // First run on a fresh checkout: write the reference and pass. Commit the file.
        FILE* f = std::fopen (goldenPath().c_str(), "w");
        REQUIRE (f != nullptr);
        std::fprintf (f, "# Sample-exact FNV-1a hashes of a fixed phrase rendered through each factory\n"
                         "# preset. Regenerate ONLY for an intended, reviewed sound change.\n"
                         "# The exact comparison runs only on the toolchain named below — the last bits\n"
                         "# of std::tan/pow/exp legitimately differ elsewhere. See the test's header.\n"
                         "#toolchain %s\n", toolchainTag().c_str());
        for (auto& kv : got) std::fprintf (f, "%s\t%llu\n", kv.first.c_str(), (unsigned long long) kv.second);
        std::fclose (f);
        WARN ("wrote a fresh preset render reference to " << goldenPath());
        return;
    }

    if (ref.tag != toolchainTag())
    {
        // Not the build that produced the table: the renders above still had to be finite
        // and audible, but the last-bit comparison would be measuring libm, not this repo.
        WARN ("preset render reference was captured on '" << ref.tag << "'; this build is '"
              << toolchainTag() << "' — rendered and sanity-checked, exact comparison skipped");
        return;
    }

    for (auto& kv : got)
    {
        INFO ("preset: " << kv.first);
        const auto it = ref.hashes.find (kv.first);
        REQUIRE (it != ref.hashes.end());             // reference covers every probe
        REQUIRE (kv.second == it->second);            // ...and matches it exactly
    }
}
