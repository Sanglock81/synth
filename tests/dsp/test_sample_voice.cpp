// ============================================================================
// I2 — stereo, pitch-tracked, one-shot SampleVoice (kit sample pads). JUCE-free.
// Proves playback, pitch ratio, stereo preservation, choke fade, anti-click ends,
// and the engine path (a KitData pad whose isSample=true → kitNoteOn → stereo out).
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "SampleVoice.h"
#include "SynthEngine.h"
#include "test_util.h"
#include "alloc_hook.h"
#include <vector>
#include <cmath>

namespace
{
    constexpr double kSR = 48000.0;
    constexpr double kPi = 3.14159265358979323846;

    // A stereo test sample: L = sine at fHz, R = the same sine at HALF amplitude (so L≠R,
    // testing that the two channels stay independent). `nativeSR` lets us also exercise SR ratio.
    void makeSine (std::vector<float>& L, std::vector<float>& R, double fHz, int len, double nativeSR)
    {
        L.assign ((std::size_t) len, 0.0f); R.assign ((std::size_t) len, 0.0f);
        for (int i = 0; i < len; ++i)
        {
            const float s = (float) std::sin (2.0 * kPi * fHz * i / nativeSR);
            L[(std::size_t) i] = s;
            R[(std::size_t) i] = 0.5f * s;
        }
    }

    // Render a voice fully into stereo buffers.
    void renderVoice (SampleVoice& v, std::vector<float>& outL, std::vector<float>& outR, int n)
    {
        outL.assign ((std::size_t) n, 0.0f); outR.assign ((std::size_t) n, 0.0f);
        v.render (outL.data(), outR.data(), n);
    }
}

TEST_CASE ("SampleVoice plays a one-shot then goes idle", "[dsp][sample]")
{
    std::vector<float> L, R; makeSine (L, R, 440.0, 4800, kSR);   // 0.1 s
    SampleVoice v; v.prepare (kSR);
    SamplePlay sp { L.data(), R.data(), (int) L.size(), kSR, /*root*/60, /*gain*/1.0f };
    v.noteOn (sp, 60, 1, /*part*/1, /*slot*/0, false);
    REQUIRE (v.isActive());

    std::vector<float> oL, oR; renderVoice (v, oL, oR, 6000);     // longer than the sample
    REQUIRE (tu::rms (oL) > 0.1);                                  // it sounded
    REQUIRE (tu::allFinite (oL));
    REQUIRE_FALSE (v.isActive());                                  // ended within the block
}

TEST_CASE ("SampleVoice is pitch-tracked: +12 st reads twice as fast", "[dsp][sample]")
{
    std::vector<float> L, R; makeSine (L, R, 300.0, 24000, kSR);   // 0.5 s @ 300 Hz, root 60
    auto play = [&] (int note)
    {
        SampleVoice v; v.prepare (kSR);
        SamplePlay sp { L.data(), R.data(), (int) L.size(), kSR, 60, 1.0f };
        v.noteOn (sp, note, 1, 1, 0, false);
        std::vector<float> oL, oR; renderVoice (v, oL, oR, 8000);
        return tu::zeroCrossHz (oL, 500, 4000, kSR);
    };
    REQUIRE (play (60) == Catch::Approx (300.0).margin (8.0));     // native pitch
    REQUIRE (play (72) == Catch::Approx (600.0).margin (16.0));    // +1 octave -> 2x
    REQUIRE (play (48) == Catch::Approx (150.0).margin (8.0));     // -1 octave -> 0.5x
}

TEST_CASE ("SampleVoice preserves the stereo image (L != R)", "[dsp][sample]")
{
    std::vector<float> L, R; makeSine (L, R, 440.0, 4800, kSR);   // R = 0.5*L by construction
    SampleVoice v; v.prepare (kSR);
    SamplePlay sp { L.data(), R.data(), (int) L.size(), kSR, 60, 1.0f };
    v.noteOn (sp, 60, 1, 1, 0, false);
    std::vector<float> oL, oR; renderVoice (v, oL, oR, 4000);
    REQUIRE (tu::rms (oL) > 0.1);
    REQUIRE (tu::rms (oR) == Catch::Approx (0.5 * tu::rms (oL)).margin (0.02));   // channel independence
}

TEST_CASE ("SampleVoice: SR mismatch is corrected by the ratio", "[dsp][sample]")
{
    // A 24 kHz-native sample of a 300 Hz sine, played on a 48 kHz engine at root, must still
    // sound at 300 Hz (ratio folds nativeSR/engineSR = 0.5).
    std::vector<float> L, R; makeSine (L, R, 300.0, 12000, 24000.0);
    SampleVoice v; v.prepare (kSR);
    SamplePlay sp { L.data(), R.data(), (int) L.size(), 24000.0, 60, 1.0f };
    v.noteOn (sp, 60, 1, 1, 0, false);
    std::vector<float> oL, oR; renderVoice (v, oL, oR, 16000);
    REQUIRE (tu::zeroCrossHz (oL, 500, 4000, kSR) == Catch::Approx (300.0).margin (8.0));
}

TEST_CASE ("SampleVoice: choke (steal) fades to silence quickly + click-free", "[dsp][sample]")
{
    std::vector<float> L, R; makeSine (L, R, 200.0, 48000, kSR);   // 1 s, long enough to choke mid-play
    SampleVoice v; v.prepare (kSR);
    SamplePlay sp { L.data(), R.data(), (int) L.size(), kSR, 60, 1.0f };
    v.noteOn (sp, 60, 1, 1, 0, false);
    std::vector<float> oL, oR; renderVoice (v, oL, oR, 2000);      // ~42 ms
    v.steal();                                                     // choke
    std::vector<float> cL, cR; renderVoice (v, cL, cR, 2000);      // fade should complete here
    REQUIRE (tu::maxDelta (cL) < 0.1f);                            // no click during the choke fade
    REQUIRE_FALSE (v.isActive());                                  // fully choked within ~4 ms
}

TEST_CASE ("SampleVoice: no click at the end of the buffer", "[dsp][sample]")
{
    // A sample truncated at a non-zero value would click at len without the tail fade.
    std::vector<float> L ((std::size_t) 2400, 0.9f), R ((std::size_t) 2400, 0.9f);   // DC-ish, ends hot
    SampleVoice v; v.prepare (kSR);
    SamplePlay sp { L.data(), R.data(), (int) L.size(), kSR, 60, 1.0f };
    v.noteOn (sp, 60, 1, 1, 0, false);
    std::vector<float> oL, oR; renderVoice (v, oL, oR, 3000);
    REQUIRE (tu::maxDelta (oL) < 0.1f);                            // fade-in + fade-out keep steps small
    REQUIRE (tu::allFinite (oL));
}

// ---- engine path: a kit pad flagged isSample plays through the sample bus ----
TEST_CASE ("engine: a sample kit pad renders through the stereo sample bus", "[dsp][sample][engine]")
{
    static std::vector<float> L, R; makeSine (L, R, 330.0, 9600, kSR);   // static: outlives the engine

    SynthEngine eng; eng.prepare (kSR, 512);
    KitData kd; kd.isKit = true;
    kd.pads[0].triggerNote = 36; kd.pads[0].soundNote[0] = 60; kd.pads[0].numSound = 1;
    kd.pads[0].isSample = true; kd.pads[0].sampleL = L.data(); kd.pads[0].sampleR = R.data();
    kd.pads[0].sampleLen = (int) L.size(); kd.pads[0].sampleSR = kSR;
    kd.pads[0].sampleRoot = 60; kd.pads[0].sampleGain = 1.0f;
    eng.setPartKit (1, kd);

    std::vector<float> mL (512), mR (512);
    FXParams fx[SynthEngine::maxParts] {};
    eng.beginMasterBlock (512, VoiceParams{}, fx[0], PartLfos{}, 0);
    eng.kitNoteOn (1, 36, 1.0f);                       // hit the sample pad
    eng.renderParts (0, 512, VoiceParams{});
    eng.mixParts (mL.data(), mR.data(), 512);
    REQUIRE (tu::rms (mL) > 0.01);                     // the sample reached the master
    const double ratio = tu::rms (mR) / tu::rms (mL);  // R was built at half L amplitude
    REQUIRE (ratio > 0.4);
    REQUIRE (ratio < 0.6);                             // stereo image preserved through the bus
    REQUIRE (tu::allFinite (mL));
}

TEST_CASE ("engine: sample-pad trigger + render allocates nothing on the audio thread", "[dsp][sample][rt]")
{
    static std::vector<float> L, R; makeSine (L, R, 250.0, 24000, kSR);
    SynthEngine eng; eng.prepare (kSR, 512);
    KitData kd; kd.isKit = true;
    for (int p = 0; p < 4; ++p)   // four sample pads across a choke group to exercise steal + choke
    {
        kd.pads[(std::size_t) p].triggerNote = 36 + p; kd.pads[(std::size_t) p].soundNote[0] = 60;
        kd.pads[(std::size_t) p].isSample = true; kd.pads[(std::size_t) p].sampleL = L.data();
        kd.pads[(std::size_t) p].sampleR = R.data(); kd.pads[(std::size_t) p].sampleLen = (int) L.size();
        kd.pads[(std::size_t) p].sampleSR = kSR; kd.pads[(std::size_t) p].sampleGain = 1.0f;
        kd.pads[(std::size_t) p].chokeGroup = 1;
    }
    eng.setPartKit (1, kd);
    std::vector<float> mL (512), mR (512);
    FXParams fx[SynthEngine::maxParts] {};
    // Warm up outside the guard (first block may touch lazy state).
    eng.beginMasterBlock (512, VoiceParams{}, fx[0], PartLfos{}, 0);
    eng.kitNoteOn (1, 36, 1.0f); eng.renderParts (0, 512, VoiceParams{}); eng.mixParts (mL.data(), mR.data(), 512);

    {
        alloc_hook::AllocGuard g;
        for (int b = 0; b < 200; ++b)
        {
            eng.beginMasterBlock (512, VoiceParams{}, fx[0], PartLfos{}, 0);
            eng.kitNoteOn (1, 36 + (b & 3), 1.0f);           // retrigger + cross-pad choke each block
            eng.renderParts (0, 512, VoiceParams{});
            eng.mixParts (mL.data(), mR.data(), 512);
        }
        REQUIRE (g.count() == 0);
    }
    REQUIRE (tu::allFinite (mL));
}

TEST_CASE ("engine: re-baking the kit mid-playback stays clean (use-after-free guard)", "[dsp][sample][rt]")
{
    // The buffer is immutable + never freed; a voice holds a borrowed pointer captured at trigger.
    // Republishing the kit repeatedly while a long sample plays must not tear or dangle. ASan (in
    // the sanitizer gate) proves the memory safety; here we assert the audio stays finite + present.
    static std::vector<float> L, R; makeSine (L, R, 180.0, 48000, kSR);   // 1 s long sample
    SynthEngine eng; eng.prepare (kSR, 256);
    auto build = [&] {
        KitData k; k.isKit = true;
        k.pads[0].triggerNote = 36; k.pads[0].soundNote[0] = 60; k.pads[0].isSample = true;
        k.pads[0].sampleL = L.data(); k.pads[0].sampleR = R.data(); k.pads[0].sampleLen = (int) L.size();
        k.pads[0].sampleSR = kSR; k.pads[0].sampleGain = 1.0f;
        return k;
    };
    eng.setPartKit (1, build());
    std::vector<float> mL (256), mR (256);
    FXParams fx[SynthEngine::maxParts] {};
    eng.beginMasterBlock (256, VoiceParams{}, fx[0], PartLfos{}, 0);
    eng.kitNoteOn (1, 36, 1.0f);                              // start the long sample
    eng.renderParts (0, 256, VoiceParams{}); eng.mixParts (mL.data(), mR.data(), 256);

    double energy = 0.0;
    for (int b = 0; b < 60; ++b)                              // ~0.32 s, well within the 1 s sample
    {
        eng.setPartKit (1, build());                         // republish the kit mid-playback
        eng.beginMasterBlock (256, VoiceParams{}, fx[0], PartLfos{}, 0);
        eng.renderParts (0, 256, VoiceParams{}); eng.mixParts (mL.data(), mR.data(), 256);
        REQUIRE (tu::allFinite (mL));
        energy += tu::rms (mL);
    }
    REQUIRE (energy > 0.0);                                   // the sample kept playing across re-bakes
}
