// ============================================================================
// Factory-preset velocity brightness (#54, category-aware vel->cutoff): every factory
// patch that routes velocity to the filter must get BRIGHTER on a harder hit, and the
// deliberately-flat categories (organ; drums route velocity to level only) must not.
// Renders each patch's baked VoiceParams through the engine (no FX) at a soft vs hard
// velocity and compares the spectral centroid — the automatable form of "verify by
// spectrogram that brightness tracks".
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginProcessor.h"   // transitively provides SynthEngine + VoiceParams
#include "test_util.h"
#include <vector>

namespace
{
    constexpr double kSR = 48000.0;

    // Render one note at `vel` through a bare engine (no FX); return an early power-of-two
    // window (same index for every velocity, so only vel->cutoff differs, not the filter env).
    std::vector<float> renderVel (const VoiceParams& p, float vel, int note = 60)
    {
        SynthEngine e; e.prepare (kSR);
        e.noteOn (note, vel);
        std::vector<float> out; out.reserve (7000);
        std::vector<float> buf (512);
        for (int done = 0; done < 6144; done += 512)
        {
            std::fill (buf.begin(), buf.end(), 0.0f);
            e.render (buf.data(), 512, p, 2.0f, 0, 0.0f, 0);
            out.insert (out.end(), buf.begin(), buf.end());
        }
        return tu::slice (out, 1024, 4096);   // 2^12 window, ~21..106 ms (still ringing for plucks/bells)
    }

    double centroidHz (const std::vector<float>& x)
    {
        auto mag = tu::magnitudeSpectrum (x);
        const double binHz = kSR / double (x.size());
        double num = 0.0, den = 0.0;
        for (std::size_t k = 1; k < mag.size(); ++k) { num += double (k) * binHz * mag[k]; den += mag[k]; }
        return den > 0.0 ? num / den : 0.0;
    }
}

TEST_CASE ("factory patches with vel->cutoff get brighter on a harder hit", "[plugin][presets][velocity]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    VASynthProcessor p;
    p.prepareToPlay (kSR, 512);

    int routed = 0, flat = 0;
    for (auto& fp : p.factoryPresetLibrary().all())
    {
        p.loadFactoryPreset (fp.name);
        const VoiceParams vp = p.currentVoiceParams();

        // Kits/drums bake per-pad, and percussive patches don't hold a steady tone — the
        // engine snapshot here is the tonal live sound, so restrict the brightness probe to
        // tonal patches. Drums route velocity to level only (vel_to_cutoff == 0) by design.
        if (fp.category == "Drums") continue;

        const double soft   = centroidHz (renderVel (vp, 0.2f));
        const double bright = centroidHz (renderVel (vp, 1.0f));

        INFO ("preset " << fp.name << " (vel_to_cutoff=" << vp.velToCutoff << ")  soft=" << soft << " hard=" << bright);
        if (vp.velToCutoff > 0.05f)
        {
            REQUIRE (bright > soft * 1.05);   // a harder hit opens the filter -> higher centroid
            ++routed;
        }
        else
        {
            REQUIRE (bright == Catch::Approx (soft).epsilon (0.05));   // no vel->cutoff -> brightness flat
            ++flat;
        }
    }

    REQUIRE (routed >= 10);   // the tonal lead/bass/keys/pluck/brass/strings/winds/pad patches
    REQUIRE (flat   >= 1);    // organ (+ any FX texture) stay deliberately flat
}
