// Synth — Copyright © 2026 John L Farmer. Licensed under AGPLv3; see LICENSE.
// ============================================================================
// Equal-loudness bank check (Inc 2). The SUSTAINED, full-spectrum factory patches must
// sit within +/-4 dB of the bank median so switching patches during a set doesn't jump
// the output level. Level is matched by the per-patch TRIM (patch_trim) only — a
// transparent post-FX gain — never by re-voicing.
//
// Percussive/evolving patches (plucks, bells, drums, risers, drones) are matched by
// transient/feel, NOT integrated RMS (docs/presets.md "Loudness"), so they're outside
// this check. The classifier is principled + measurable: a patch is "sustained" when its
// amp envelope holds (ampS > 0.25) AND it is still ringing at full level late in the note
// (late window within 6 dB of the early window), and it isn't a Drums/FX patch.
//
// The check runs POST-trim (the real shipped output), so it is enforceable forever.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "PluginProcessor.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
    constexpr double kSR = 48000.0;

    struct Meas { double fullDb; double earlyDb; double lateDb; };

    double toDb (double rms) { return 20.0 * std::log10 (std::max (rms, 1.0e-9)); }

    double windowRmsDb (const std::vector<float>& mono, double t0, double t1)
    {
        const int a = (int) (kSR * t0), b = (int) (kSR * t1);
        double acc = 0.0; int cnt = 0;
        for (int i = a; i < b && i < (int) mono.size(); ++i) { acc += double (mono[(size_t) i]) * mono[(size_t) i]; ++cnt; }
        return toDb (cnt ? std::sqrt (acc / cnt) : 0.0);
    }

    // Hold middle C for 2.6 s; return full-note RMS plus early/late windows.
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
        if (category == "Drums" || category == "FX" || category == "Experimental") return false;   // matched by transient/feel
        if (ampS <= 0.25f) return false;                             // pluck/bell/clav — percussive VCA
        return (m.lateDb - m.earlyDb) > -6.0;                        // still ringing at full level -> real sustain
    }
}

TEST_CASE ("sustained factory patches are level-matched within +/-4 dB of the bank median", "[plugin][presets][loudness]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;

    struct Entry { juce::String name; double db; };
    std::vector<Entry> bank;

    for (auto& fp : p.factoryPresetLibrary().all())
    {
        p.loadFactoryPreset (fp.name);
        const float ampS = p.currentVoiceParams().ampS;
        const Meas m = measure (p);
        if (! isSustainedClass (fp.category, ampS, m)) continue;
        bank.push_back ({ fp.name, m.fullDb });
    }

    REQUIRE (bank.size() >= 20);   // the sustained bass/lead/pad/keys/brass/strings/winds/organ patches

    std::vector<double> dbs; for (auto& e : bank) dbs.push_back (e.db);
    std::sort (dbs.begin(), dbs.end());
    const double median = dbs[dbs.size() / 2];

    for (auto& e : bank)
    {
        INFO ("patch " << e.name << " = " << e.db << " dBFS  (median " << median << ", delta " << (e.db - median) << ")");
        REQUIRE (std::abs (e.db - median) <= 4.0);
    }
}
