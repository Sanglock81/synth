// Synth — Copyright © 2026 John L Farmer. Licensed under AGPLv3; see LICENSE.
// ============================================================================
// MP3 encoding for the master recorder, using the EMBEDDED libmp3lame (cmake/lame.cmake).
//
// Out-of-line on purpose: <lame.h> is included here and nowhere else, so it never reaches
// the ~dozen translation units that include PluginProcessor.h (and therefore
// MasterRecorder.h). The rest of MasterRecorder stays header-only.
//
// There is no external encoder to find and nothing for the user to install -- the earlier
// `lame`/`ffmpeg` discovery is gone, and MP3 now always works on Linux and Windows alike.
// ============================================================================
#include "MasterRecorder.h"
#include <lame.h>
#include <vector>

namespace
{
    // lame_close() must run on every exit path, including the error returns below.
    struct LameHandle
    {
        lame_global_flags* gf = lame_init();
        LameHandle() = default;        // declaring the deleted copy ctor below suppresses the implicit one
        ~LameHandle() { if (gf != nullptr) lame_close (gf); }
        LameHandle (const LameHandle&) = delete;
        LameHandle& operator= (const LameHandle&) = delete;
    };
}

bool MasterRecorder::encodeMp3 (const juce::File& dest, int kbps, juce::String& error) const
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader (
        wav.createReaderFor (take.createInputStream().release(), true));
    if (reader == nullptr) { error = "Could not read the recorded take."; return false; }
    if (reader->lengthInSamples <= 0) { error = "The recorded take is empty."; return false; }

    LameHandle lame;
    if (lame.gf == nullptr) { error = "Could not initialise the MP3 encoder."; return false; }

    const int channels = (int) juce::jlimit ((unsigned int) 1, (unsigned int) 2, reader->numChannels);
    lame_set_in_samplerate (lame.gf, (int) reader->sampleRate);
    lame_set_num_channels  (lame.gf, channels);
    lame_set_brate         (lame.gf, kbps);          // constant bitrate: what the format menu promises
    lame_set_VBR           (lame.gf, vbr_off);
    lame_set_quality       (lame.gf, 2);             // 0 = best/slowest; 2 is LAME's recommended near-best
    lame_set_bWriteVbrTag  (lame.gf, 1);             // reserve frame 0 for the LAME/Xing tag (see below)
    if (lame_init_params (lame.gf) < 0)
    { error = "The MP3 encoder rejected " + juce::String (kbps) + " kbps at "
            + juce::String ((int) reader->sampleRate) + " Hz."; return false; }

    dest.deleteFile();
    std::unique_ptr<juce::FileOutputStream> out (dest.createOutputStream());
    if (out == nullptr || ! out->openedOk())
    { error = "Could not create " + dest.getFullPathName(); return false; }

    // 1152 samples is one MPEG-1 Layer III granule pair; a multiple of it keeps LAME on
    // frame boundaries. The output bound is LAME's documented worst case for a chunk.
    const int chunk = 1152 * 16;
    juce::AudioBuffer<float> buf (juce::jmax (2, channels), chunk);
    std::vector<unsigned char> mp3 ((std::size_t) (1.25 * chunk) + 7200);

    for (juce::int64 pos = 0; pos < reader->lengthInSamples; pos += chunk)
    {
        const int n = (int) juce::jmin ((juce::int64) chunk, reader->lengthInSamples - pos);
        buf.clear();
        if (! reader->read (&buf, 0, n, pos, true, channels > 1))
        { error = "Could not read the take while encoding."; return false; }

        // LAME wants a pointer per channel; a mono take feeds the same buffer to both so the
        // encoder still sees the channel count it was configured with.
        const float* L = buf.getReadPointer (0);
        const float* R = buf.getReadPointer (channels > 1 ? 1 : 0);
        const int bytes = lame_encode_buffer_ieee_float (lame.gf, L, R, n,
                                                         mp3.data(), (int) mp3.size());
        if (bytes < 0) { error = "MP3 encoding failed (error " + juce::String (bytes) + ")."; return false; }
        if (bytes > 0 && ! out->write (mp3.data(), (std::size_t) bytes))
        { error = "Could not write " + dest.getFileName(); return false; }
    }

    const int tail = lame_encode_flush (lame.gf, mp3.data(), (int) mp3.size());
    if (tail < 0) { error = "MP3 encoding failed while flushing."; return false; }
    if (tail > 0 && ! out->write (mp3.data(), (std::size_t) tail))
    { error = "Could not write " + dest.getFileName(); return false; }

    // The LAME/Xing info tag goes in the frame LAME reserved at offset 0. Without it players
    // fall back to estimating from file size, so a take can report the wrong duration and seek
    // badly. It can only be written after encoding, because it carries the final frame count.
    const int tagSize = (int) lame_get_lametag_frame (lame.gf, mp3.data(), mp3.size());
    if (tagSize > 0 && tagSize <= (int) mp3.size())
    {
        if (! out->setPosition (0) || ! out->write (mp3.data(), (std::size_t) tagSize))
        { error = "Could not write the MP3 header tag."; return false; }
    }

    out->flush();
    const bool ok = out->getStatus().wasOk();
    out.reset();
    if (! ok || ! dest.existsAsFile() || dest.getSize() == 0)
    { error = "MP3 encoding produced no output."; return false; }
    return true;
}

juce::String MasterRecorder::mp3EncoderName()
{
    return "libmp3lame " + juce::String (get_lame_version());
}
