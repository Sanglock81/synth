// ============================================================================
// I2 (persistence) — importing a WAV into a kit pad, playing it, deduping by content,
// and round-tripping the reference through a .kit tree + a MULTI. The managed sample
// library (SampleStore) is content-addressed, so a pad carries only an md5 key.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginProcessor.h"
#include "UI/KitEditor.h"
#include <memory>
#include <cmath>

#ifndef VASYNTH_DOCS_DIR
 #define VASYNTH_DOCS_DIR "."
#endif

namespace
{
    constexpr double kSR = 48000.0;

    // Write a short stereo sine WAV to a temp file; return it. Caller deletes.
    juce::File makeWav (double fHz, int len)
    {
        auto f = juce::File::createTempFile (".wav");
        juce::WavAudioFormat wav;
        auto os = std::make_unique<juce::FileOutputStream> (f);
        std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), kSR, 2, 16, {}, 0));
        os.release();
        juce::AudioBuffer<float> buf (2, len);
        for (int i = 0; i < len; ++i)
        {
            const float s = (float) std::sin (2.0 * 3.14159265358979 * fHz * i / kSR);
            buf.setSample (0, i, s); buf.setSample (1, i, 0.5f * s);
        }
        w->writeFromAudioSampleBuffer (buf, 0, len);
        w.reset();
        return f;
    }

    double partEnergy (VASynthProcessor& p, int trig, int part)
    {
        juce::AudioBuffer<float> buf (2, 256); juce::MidiBuffer m;
        p.routeNoteOn (trig, 1.0f, part);
        double e = 0.0;
        for (int b = 0; b < 40; ++b) { buf.clear(); m.clear(); p.processBlock (buf, m);
            for (int i = 0; i < 256; ++i) e += (double) buf.getSample (0, i) * buf.getSample (0, i); }
        return e;
    }
}

TEST_CASE ("importing a WAV into a kit pad makes it play the sample", "[plugin][sample][kit]")
{
    juce::ScopedJuceInitialiser_GUI init;
    auto wav = makeWav (330.0, 9600);

    VASynthProcessor p; p.prepareToPlay (kSR, 256);
    p.setPartKit (3, VASynthProcessor::factoryKit ("808 Basics"));
    const int trig = p.getPartKit (3).pads[0].triggerNote;   // pad 0's trigger note
    const double synth = partEnergy (p, trig, 3);            // synth pad energy

    REQUIRE (p.importPadSample (3, 0, wav));
    REQUIRE (p.getPartKit (3).pads[0].samplePath.isNotEmpty());
    const double sampled = partEnergy (p, trig, 3);
    INFO ("synth=" << synth << "  sampled=" << sampled);
    REQUIRE (sampled > 0.0);                                  // the sample sounds
    REQUIRE (std::abs (sampled - synth) > synth * 0.05);      // it's a different sound than the synth pad
    wav.deleteFile();
}

TEST_CASE ("the same WAV in two pads dedupes to one buffer + one on-disk copy", "[plugin][sample][kit]")
{
    juce::ScopedJuceInitialiser_GUI init;
    auto wav = makeWav (220.0, 4800);
    VASynthProcessor p; p.prepareToPlay (kSR, 256);
    p.setPartKit (3, VASynthProcessor::factoryKit ("808 Basics"));

    REQUIRE (p.importPadSample (3, 0, wav));
    const auto key0 = p.getPartKit (3).pads[0].samplePath;
    REQUIRE (p.importPadSample (3, 1, wav));
    const auto key1 = p.getPartKit (3).pads[1].samplePath;

    REQUIRE (key0 == key1);                                   // identical content -> identical key
    const int copies = AppInfo::samplesDir().getNumberOfChildFiles (juce::File::findFiles, key0 + ".*");
    REQUIRE (copies == 1);                                    // one managed on-disk copy
    wav.deleteFile();
}

TEST_CASE ("a sample pad round-trips through a MULTI (session persistence)", "[plugin][sample][kit][multi]")
{
    juce::ScopedJuceInitialiser_GUI init;
    auto wav = makeWav (440.0, 4800);

    VASynthProcessor src; src.prepareToPlay (kSR, 256);
    src.setPartKit (3, VASynthProcessor::factoryKit ("808 Basics"));
    REQUIRE (src.importPadSample (3, 0, wav));
    const auto key = src.getPartKit (3).pads[0].samplePath;
    auto multi = src.captureMultiState();

    VASynthProcessor dst; dst.prepareToPlay (kSR, 256);
    dst.applyMultiState (multi);
    REQUIRE (dst.getPartKit (3).pads[0].samplePath == key);   // reference persisted
    REQUIRE (dst.isPartKit (3));
    // and it resolves from the managed library on the fresh processor -> it plays.
    const int trig = dst.getPartKit (3).pads[0].triggerNote;
    REQUIRE (partEnergy (dst, trig, 3) > 0.0);
    wav.deleteFile();
}

TEST_CASE ("a missing sample reference is silent, never a crash", "[plugin][sample][kit]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VASynthProcessor p; p.prepareToPlay (kSR, 256);
    auto def = VASynthProcessor::factoryKit ("808 Basics");
    def.pads[0].samplePath = "deadbeefdeadbeef";              // no such managed sample
    p.setPartKit (3, def);                                    // must not crash
    const int trig = p.getPartKit (3).pads[0].triggerNote;
    REQUIRE (partEnergy (p, trig, 3) == Catch::Approx (0.0).margin (1e-6));   // silent pad
}

TEST_CASE ("Kit Editor: a loaded sample flips the pad to SMPL (UI + screenshot)", "[plugin][sample][kit][screenshot]")
{
    juce::ScopedJuceInitialiser_GUI init;
    auto wav = makeWav (300.0, 6000);

    VASynthProcessor p; p.prepareToPlay (kSR, 256);
    p.setPartKit (3, VASynthProcessor::factoryKit ("808 Basics"));
    REQUIRE (p.importPadSample (3, 0, wav));                  // pad 1 becomes a sample
    REQUIRE (p.getPartKit (3).pads[0].samplePath.isNotEmpty());

    KitEditor ed (p, 3);
    ed.setSize (660, 560);
    auto img = ed.createComponentSnapshot (ed.getLocalBounds(), false, 1.0f);
    REQUIRE (img.isValid());
    juce::File out (juce::String (VASYNTH_DOCS_DIR) + "/smoke/kit-sample.png");
    out.getParentDirectory().createDirectory(); out.deleteFile();
    juce::FileOutputStream os (out); REQUIRE (os.openedOk());
    juce::PNGImageFormat png; REQUIRE (png.writeImageToStream (img, os));
    wav.deleteFile();
}
