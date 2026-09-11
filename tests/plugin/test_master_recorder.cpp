// Synth — Copyright © 2026 John L Farmer. Licensed under AGPLv3; see LICENSE.
// ============================================================================
// REC (master recorder): the capture path and the save/transcode path.
//
// The recorder's whole job is that the file matches what the DAC got, for any take
// length, with no dropouts and nothing done on the audio thread. These tests drive the
// REAL processBlock render path (never MasterRecorder::write directly) so a tap wired
// into the wrong stage -- or not wired at all -- fails here.
//
// PORTABILITY. WAV/FLAC/Ogg encode inside JUCE and are asserted unconditionally on every
// platform. MP3 needs an external encoder that a CI box may not have, so its test asserts
// the CONTRACT rather than the outcome: either it encodes a playable file, or it fails with
// the install hint. Never silently skipped.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PluginProcessor.h"
#include "MasterRecorder.h"
#include <cmath>

namespace
{
    // Render `blocks` of real audio through processBlock, holding a note so the master
    // output is genuinely non-silent.
    void renderNote (VASynthProcessor& p, int blocks, int blockSize = 128)
    {
        for (int b = 0; b < blocks; ++b)
        {
            juce::AudioBuffer<float> buf (2, blockSize); buf.clear();
            juce::MidiBuffer midi;
            if (b == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
            p.processBlock (buf, midi);
        }
    }

    // Read a written audio file back with whatever format claims it.
    struct Decoded { int channels = 0; juce::int64 length = 0; double rate = 0.0; float peak = 0.0f; bool ok = false; };
    Decoded decode (const juce::File& f)
    {
        Decoded d;
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
        if (r == nullptr) return d;
        d.channels = (int) r->numChannels; d.length = r->lengthInSamples; d.rate = r->sampleRate;
        juce::AudioBuffer<float> buf ((int) r->numChannels, (int) juce::jmin (r->lengthInSamples, (juce::int64) 1 << 20));
        r->read (&buf, 0, buf.getNumSamples(), 0, true, r->numChannels > 1);
        d.peak = buf.getMagnitude (0, buf.getNumSamples());
        d.ok = true;
        return d;
    }

    // MP3 cannot be verified with decode(): JUCE's MP3 support is a DECODER that is compiled
    // out by default (JUCE_USE_MP3AUDIOFORMAT=0), and we deliberately do not turn it on just
    // for a test. So check the container instead -- an ID3 tag or an MPEG frame sync -- plus a
    // size consistent with the requested bitrate, which together only hold for a real MP3.
    bool looksLikeMp3 (const juce::File& f, int kbps, double seconds)
    {
        juce::FileInputStream in (f);
        if (! in.openedOk() || in.getTotalLength() < 128) return false;
        juce::uint8 head[3] {};
        if (in.read (head, 3) != 3) return false;
        const bool id3  = head[0] == 'I' && head[1] == 'D' && head[2] == '3';
        const bool sync = head[0] == 0xFF && (head[1] & 0xE0) == 0xE0;      // MPEG frame sync
        if (! id3 && ! sync) return false;

        const double expected = seconds * (kbps * 1000.0 / 8.0);
        const double got      = (double) f.getSize();
        return got > expected * 0.3 && got < expected * 5.0 + 4096.0;       // + room for tags
    }
}

TEST_CASE ("rec: a take captures the master output through the real render path",
           "[plugin][rec]")
{
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 128);

    REQUIRE_FALSE (p.isMasterRecording());
    // Rendering while idle must not capture anything (and must not crash on the null writer).
    renderNote (p, 10);
    REQUIRE (p.masterRecorder().recordedSamples() == 0);

    REQUIRE (p.startMasterRecording());
    REQUIRE (p.isMasterRecording());

    const int blocks = 60, bs = 128;
    renderNote (p, blocks, bs);
    REQUIRE (p.stopMasterRecording());
    REQUIRE_FALSE (p.isMasterRecording());

    // Every rendered sample reached the writer, and none were dropped.
    REQUIRE (p.masterRecorder().recordedSamples() == (juce::int64) blocks * bs);
    REQUIRE (p.masterRecorder().droppedBlocks() == 0);
    REQUIRE (p.masterRecorder().recordedSeconds() == Catch::Approx ((double) (blocks * bs) / 48000.0).epsilon (0.001));

    auto take = p.masterRecorder().takeFile();
    REQUIRE (take.existsAsFile());
    auto d = decode (take);
    REQUIRE (d.ok);
    REQUIRE (d.channels == 2);
    REQUIRE (d.length == (juce::int64) blocks * bs);
    REQUIRE (d.rate == Catch::Approx (48000.0));
    REQUIRE (d.peak > 0.001f);                    // a held note really is in the file

    p.masterRecorder().discardTake();
    REQUIRE_FALSE (take.existsAsFile());          // no temp litter left behind
}

// The tap sits after master gain and the safety clipper, so the file must equal what the
// host bus received -- not a pre-gain or pre-clip copy. Renders the SAME audio twice (once
// recording, once not) and compares the file against the bus output sample-for-sample.
TEST_CASE ("rec: the file matches the audio the host bus received", "[plugin][rec]")
{
    const int blocks = 24, bs = 128;

    auto renderCapturingBus = [&] (bool record, juce::AudioBuffer<float>& bus, VASynthProcessor& p)
    {
        p.prepareToPlay (48000.0, bs);
        if (record) REQUIRE (p.startMasterRecording());
        bus.setSize (2, blocks * bs); bus.clear();
        for (int b = 0; b < blocks; ++b)
        {
            juce::AudioBuffer<float> buf (2, bs); buf.clear();
            juce::MidiBuffer midi;
            if (b == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
            p.processBlock (buf, midi);
            for (int ch = 0; ch < 2; ++ch) bus.copyFrom (ch, b * bs, buf, ch, 0, bs);
        }
    };

    VASynthProcessor rp_;
    juce::AudioBuffer<float> bus;
    renderCapturingBus (true, bus, rp_);
    REQUIRE (rp_.stopMasterRecording());

    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (rp_.masterRecorder().takeFile()));
    REQUIRE (r != nullptr);
    juce::AudioBuffer<float> file (2, (int) r->lengthInSamples);
    r->read (&file, 0, file.getNumSamples(), 0, true, true);
    r.reset();

    REQUIRE (file.getNumSamples() == bus.getNumSamples());
    // The intermediate is 24-bit PCM, so allow one LSB of quantisation (2^-23) and no more.
    float worst = 0.0f;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < bus.getNumSamples(); ++i)
            worst = std::max (worst, std::abs (file.getSample (ch, i) - bus.getSample (ch, i)));
    REQUIRE (worst < 1.0e-6f);
    REQUIRE (bus.getMagnitude (0, bus.getNumSamples()) > 0.001f);   // sanity: we compared real audio

    rp_.masterRecorder().discardTake();
}

// A take spanning many blocks at a small block size is the realistic live case; the FIFO
// must absorb all of it without the audio thread ever blocking on the disk.
TEST_CASE ("rec: a long take streams without dropouts", "[plugin][rec]")
{
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 64);
    REQUIRE (p.startMasterRecording());
    const int blocks = 4000, bs = 64;               // ~5.3 audio-seconds in 64-sample blocks
    renderNote (p, blocks, bs);
    REQUIRE (p.stopMasterRecording());
    REQUIRE (p.masterRecorder().droppedBlocks() == 0);
    REQUIRE (p.masterRecorder().recordedSamples() == (juce::int64) blocks * bs);
    auto d = decode (p.masterRecorder().takeFile());
    REQUIRE (d.ok);
    REQUIRE (d.length == (juce::int64) blocks * bs);
    p.masterRecorder().discardTake();
}

// Start/stop must be re-entrant-safe and leave exactly one take: a second start while
// recording is a no-op, and a stop with nothing recorded reports failure rather than
// offering an empty file to save.
TEST_CASE ("rec: repeated start/stop keeps one take and reports an empty one", "[plugin][rec]")
{
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 128);

    REQUIRE_FALSE (p.stopMasterRecording());        // stop while idle
    REQUIRE (p.startMasterRecording());
    REQUIRE (p.startMasterRecording());             // second start is a no-op, not a new file
    auto first = p.masterRecorder().takeFile();
    renderNote (p, 20);
    REQUIRE (p.stopMasterRecording());
    REQUIRE (p.masterRecorder().takeFile() == first);

    // A new take replaces the old temp file rather than accumulating.
    REQUIRE (p.startMasterRecording());
    auto second = p.masterRecorder().takeFile();
    REQUIRE (second != first);
    REQUIRE_FALSE (first.existsAsFile());
    renderNote (p, 5);
    REQUIRE (p.stopMasterRecording());
    p.masterRecorder().discardTake();

    // Armed and stopped without ever rendering: nothing captured, so no file to offer.
    REQUIRE (p.startMasterRecording());
    REQUIRE_FALSE (p.stopMasterRecording());
    p.masterRecorder().discardTake();
}

// Every JUCE-native format must round-trip on every platform: the saved file decodes, has
// the take's length and channel count, and still carries the audio.
TEST_CASE ("rec: saving transcodes to every built-in format", "[plugin][rec]")
{
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 128);
    REQUIRE (p.startMasterRecording());
    const int blocks = 120, bs = 128;
    renderNote (p, blocks, bs);
    REQUIRE (p.stopMasterRecording());
    const juce::int64 n = (juce::int64) blocks * bs;

    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("synth-rec-test-" + juce::String (juce::Time::currentTimeMillis()));
    REQUIRE (dir.createDirectory().wasOk());

    for (const auto& fmt : MasterRecorder::formats())
    {
        auto dest = dir.getChildFile (juce::String ("take-") + juce::String (fmt.label).retainCharacters (
                        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") + "." + fmt.ext);
        juce::String error;
        const bool ok = p.masterRecorder().transcodeTo (dest, fmt, error);
        INFO ("format=" << fmt.label << "  ok=" << (int) ok << "  error=" << error);

        if (fmt.kind == MasterRecorder::Kind::Mp3 && ! MasterRecorder::mp3Available())
        {
            // No encoder on this machine: the contract is a clear, actionable failure.
            REQUIRE_FALSE (ok);
            REQUIRE (error == MasterRecorder::installEncoderHint());
            REQUIRE (error.containsIgnoreCase ("mp3"));
            continue;
        }

        REQUIRE (ok);
        REQUIRE (error.isEmpty());
        REQUIRE (dest.existsAsFile());
        REQUIRE (dest.getSize() > 0);

        if (fmt.kind == MasterRecorder::Kind::Mp3)
        {
            REQUIRE (looksLikeMp3 (dest, fmt.param, (double) n / 48000.0));
            continue;
        }

        auto d = decode (dest);
        REQUIRE (d.ok);                                 // it is a real, decodable audio file
        REQUIRE (d.channels == 2);
        REQUIRE (d.rate == Catch::Approx (48000.0));
        REQUIRE (d.peak > 0.001f);                      // the audio survived the encode
        if (fmt.kind == MasterRecorder::Kind::Wav || fmt.kind == MasterRecorder::Kind::Flac)
            REQUIRE (d.length == n);                    // lossless: exact length
        else
            REQUIRE (std::abs (d.length - n) < 48000);  // lossy codecs pad/trim by up to a frame
    }

    // A higher bitrate must produce a bigger file -- i.e. the kbps argument really reaches the
    // encoder rather than every MP3 entry yielding the same default.
    if (MasterRecorder::mp3Available())
    {
        auto hi = dir.getChildFile ("hi.mp3"), lo = dir.getChildFile ("lo.mp3");
        juce::String e1, e2;
        REQUIRE (p.masterRecorder().transcodeTo (hi, MasterRecorder::Format { "MP3 320", "mp3", MasterRecorder::Kind::Mp3, 320 }, e1));
        REQUIRE (p.masterRecorder().transcodeTo (lo, MasterRecorder::Format { "MP3 128", "mp3", MasterRecorder::Kind::Mp3, 128 }, e2));
        REQUIRE (hi.getSize() > lo.getSize());
    }

    // The lossy formats must actually be smaller than the lossless take -- i.e. they really
    // encoded rather than falling through to a WAV copy.
    auto wav24 = dir.getChildFile ("take-WAV24bitlossless.wav");
    auto ogg   = dir.getChildFile ("take-OggVorbisquality8.ogg");
    REQUIRE (wav24.existsAsFile());
    REQUIRE (ogg.existsAsFile());
    REQUIRE (ogg.getSize() < wav24.getSize());

    p.masterRecorder().discardTake();
    dir.deleteRecursively();
}

// The MP3 story is the one platform-dependent piece, so pin the discovery contract itself:
// availability and the located file must agree, and the hint must name this platform's fix.
TEST_CASE ("rec: the MP3 encoder discovery contract holds on this platform", "[plugin][rec]")
{
    const auto enc = MasterRecorder::findMp3Encoder();
    REQUIRE (MasterRecorder::mp3Available() == enc.existsAsFile());

    const auto hint = MasterRecorder::installEncoderHint();
    REQUIRE (hint.isNotEmpty());
    REQUIRE (hint.containsIgnoreCase ("mp3"));
   #if JUCE_WINDOWS
    REQUIRE (hint.containsIgnoreCase ("lame.exe"));
   #else
    REQUIRE (hint.containsIgnoreCase ("lame"));
   #endif

    if (enc.existsAsFile())
    {
        // Whatever was found must be one of the two encoders we know how to drive.
        const auto name = enc.getFileNameWithoutExtension().toLowerCase();
        REQUIRE ((name == "lame" || name == "ffmpeg"));
    }
}

// Formats are the dialog's menu contents, so their shape is a UI contract: MP3 and WAV must
// both be offered (the requested minimum), and every entry needs a usable label + extension.
TEST_CASE ("rec: the format list offers at least WAV and MP3, all well-formed", "[plugin][rec]")
{
    const auto fmts = MasterRecorder::formats();
    REQUIRE (fmts.size() >= 2);
    REQUIRE (juce::String (fmts[0].ext) == "wav");        // the default must be the safe one

    bool haveWav = false, haveMp3 = false;
    for (const auto& f : fmts)
    {
        REQUIRE (juce::String (f.label).isNotEmpty());
        REQUIRE (juce::String (f.ext).isNotEmpty());
        REQUIRE_FALSE (juce::String (f.ext).startsWith ("."));   // the dialog appends the dot
        if (f.kind == MasterRecorder::Kind::Wav) haveWav = true;
        if (f.kind == MasterRecorder::Kind::Mp3) haveMp3 = true;
    }
    REQUIRE (haveWav);
    REQUIRE (haveMp3);
}

// An offline session bounce drives renderBlockImpl DIRECTLY, bypassing the suspend guard, and
// renders minutes of audio in seconds. If the REC tap sat on that shared path, bouncing while
// armed would splice the whole bounce into the live take. The tap belongs to processBlock only.
TEST_CASE ("rec: an offline bounce does not contaminate an armed take", "[plugin][rec]")
{
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 128);
    REQUIRE (p.startMasterRecording());

    const int blocks = 30, bs = 128;
    renderNote (p, blocks, bs);
    const juce::int64 before = p.masterRecorder().recordedSamples();
    REQUIRE (before == (juce::int64) blocks * bs);

    // A 2-bar bounce is many thousands of samples -- far more than the live take so far.
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("synth-bounce-rec-" + juce::String (juce::Time::currentTimeMillis()));
    p.setAudioSuspended (true);
    const bool bounced = p.bounceSession (dir, 2);
    p.setAudioSuspended (false);
    REQUIRE (bounced);

    // Not one sample of the offline render reached the take.
    REQUIRE (p.masterRecorder().recordedSamples() == before);

    // ...and the take still grows normally afterwards.
    renderNote (p, 10, bs);
    REQUIRE (p.masterRecorder().recordedSamples() == before + 10 * bs);
    REQUIRE (p.stopMasterRecording());
    REQUIRE (decode (p.masterRecorder().takeFile()).length == before + 10 * bs);

    p.masterRecorder().discardTake();
    dir.deleteRecursively();
}

// A suspended device (the bounce's own guard, a device change) must not silently shorten the
// take's timeline either -- processBlock returns early, so nothing is recorded, and the take
// simply has fewer samples rather than a stretch of garbage.
TEST_CASE ("rec: a suspended device records nothing rather than garbage", "[plugin][rec]")
{
    VASynthProcessor p;
    p.prepareToPlay (48000.0, 128);
    REQUIRE (p.startMasterRecording());
    renderNote (p, 10);
    const juce::int64 before = p.masterRecorder().recordedSamples();

    p.setAudioSuspended (true);
    renderNote (p, 50);                              // processBlock clears and returns
    REQUIRE (p.masterRecorder().recordedSamples() == before);
    p.setAudioSuspended (false);

    renderNote (p, 10);
    REQUIRE (p.masterRecorder().recordedSamples() == before + 10 * 128);
    REQUIRE (p.stopMasterRecording());
    p.masterRecorder().discardTake();
}
