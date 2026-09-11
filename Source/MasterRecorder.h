// Synth — Copyright © 2026 John L Farmer. Licensed under AGPLv3; see LICENSE.
#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>

// ============================================================================
// MASTER RECORDER — the top bar's REC/STOP button.
//
// Captures the FINAL master output (post master-gain, post safety-clipper: exactly what
// leaves the DAC, so a take sounds like what was played) for an unbounded length, then
// hands the finished take to a save dialog that transcodes it to the user's format.
//
// Two threads, one handshake:
//
//   audio thread ── write() ──> ThreadedWriter's lock-free FIFO
//                                        │
//   writer thread (TimeSliceThread) ──────┴──> a 24-bit WAV in the temp dir
//
//   STOP ──> the take file ──> transcodeTo() ──> the user's chosen format
//
// Why a temp WAV and not RAM: a take has no length limit, and float stereo at 48k costs
// ~23 MB/minute, so an hour-long session held in RAM would be ~1.4 GB and lost on a crash.
// Streaming costs the same disk either way and the take survives.
//
// Why 24-bit for the intermediate: the master is already clipped to +/-1.0, so 24 bits
// (144 dB) is lossless in practice, and it matches the existing bounce/export writers.
//
// RT-SAFETY. The audio thread never allocates, never locks and never touches the file.
// JUCE's own recording demo guards the writer with a CriticalSection taken on the audio
// thread; we use a lock-free Dekker handshake instead (`inWrite` vs `active`, both
// seq_cst) so a message-thread stop can never priority-invert the audio thread. stop()
// clears `active`, waits for the audio thread to leave write(), and only then destroys
// the writer.
//
// PORTABILITY. Linux and Windows share this file. WAV / FLAC / Ogg Vorbis encode inside
// JUCE, so they always work on both. MP3 is the exception: JUCE ships an MP3 *decoder*
// only, so it needs an external encoder (`lame`, else `ffmpeg`), located at save time by
// findMp3Encoder() — see that function for the search order and the Windows story.
// ============================================================================
class MasterRecorder
{
public:
    MasterRecorder() = default;
    ~MasterRecorder() { stop(); discardTake(); }

    // ---- target formats -------------------------------------------------------------
    // `param` is bits for WAV/FLAC, the Ogg quality-option index for OGG, and kbps for MP3.
    enum class Kind { Wav, Flac, Ogg, Mp3 };
    struct Format
    {
        const char* label;      // what the dialog's picker shows
        const char* ext;        // file extension, no dot
        Kind        kind;
        int         param;
    };

    // The picker's contents, in order. WAV first (the safe default: every DAW eats it).
    static juce::Array<Format> formats()
    {
        return { Format { "WAV 24-bit (lossless)", "wav",  Kind::Wav,  24 },
                 Format { "WAV 16-bit (CD)",       "wav",  Kind::Wav,  16 },
                 Format { "FLAC (lossless)",       "flac", Kind::Flac, 24 },
                 Format { "Ogg Vorbis (quality 8)","ogg",  Kind::Ogg,   8 },
                 Format { "MP3 320 kbps",          "mp3",  Kind::Mp3, 320 },
                 Format { "MP3 192 kbps",          "mp3",  Kind::Mp3, 192 } };
    }

    // ================================ message thread ================================

    // Open a new take. Returns false (and records nothing) if the temp file can't be
    // created or the sample rate is nonsense. A second call while recording is a no-op.
    bool start (double sampleRate, int numChannels = 2)
    {
        if (isRecording()) return true;
        if (sampleRate <= 0.0) return false;
        discardTake();                                  // one take at a time

        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory);
        take = dir.getChildFile ("synth-take-" + juce::String (juce::Time::currentTimeMillis()) + ".wav");
        take.deleteFile();
        auto os = take.createOutputStream();
        if (os == nullptr) { take = juce::File(); return false; }

        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> w (
            wav.createWriterFor (os.get(), sampleRate, (unsigned int) juce::jmax (1, numChannels), 24, {}, 0));
        if (w == nullptr) { take.deleteFile(); take = juce::File(); return false; }
        os.release();                                   // the writer owns the stream now

        // The FIFO must absorb a whole disk stall, not just one block: 8 seconds of audio.
        // Undersizing it here is what makes a recording drop samples under load.
        thread.startThread (juce::Thread::Priority::normal);
        writer = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (
                     w.release(), thread, (int) (sampleRate * 8.0));

        rate = sampleRate;
        recorded.store (0, std::memory_order_relaxed);
        dropped.store  (0, std::memory_order_relaxed);
        active.store (writer.get(), std::memory_order_seq_cst);   // audio thread starts feeding here
        return true;
    }

    // Close the take and flush it to disk. The file stays on disk for transcodeTo();
    // returns false if there was no recording or it captured nothing.
    bool stop()
    {
        if (writer == nullptr) return false;

        // Lock-free teardown: retire the pointer, then wait for the audio thread to leave
        // write() before destroying what it was writing to. The audio thread's exit is
        // bounded by one block, so this settles in microseconds; the loop is a safety net
        // for a stalled/suspended device, not the expected path.
        active.store (nullptr, std::memory_order_seq_cst);
        for (int i = 0; i < 2000 && inWrite.load (std::memory_order_seq_cst) != 0; ++i)
            juce::Thread::sleep (1);

        writer.reset();                                 // flushes the FIFO, then closes the file
        thread.stopThread (4000);
        return take.existsAsFile() && recordedSamples() > 0;
    }

    bool   isRecording()     const noexcept { return active.load (std::memory_order_seq_cst) != nullptr; }
    juce::int64 recordedSamples() const noexcept { return recorded.load (std::memory_order_relaxed); }
    double recordedSeconds() const noexcept { return rate > 0.0 ? (double) recordedSamples() / rate : 0.0; }
    // Blocks the writer FIFO could not accept (disk could not keep up) — a dropout the user
    // should be told about rather than left to discover in the file.
    int    droppedBlocks()   const noexcept { return dropped.load (std::memory_order_relaxed); }
    juce::File takeFile()    const noexcept { return take; }
    double sampleRate()      const noexcept { return rate; }

    // Delete the temp take. Called after a successful save, on discard, and at shutdown so
    // an abandoned take never lingers in the temp dir.
    void discardTake()
    {
        if (take != juce::File() && take.existsAsFile()) take.deleteFile();
        take = juce::File();
    }

    // ================================ audio thread ===================================
    // RT-safe: no allocation, no locks, no file I/O. A no-op unless armed.
    void write (const float* L, const float* R, int numSamples) noexcept
    {
        if (numSamples <= 0) return;
        inWrite.store (1, std::memory_order_seq_cst);
        if (auto* w = active.load (std::memory_order_seq_cst))
        {
            const float* chans[2] { L, R };
            if (w->write (chans, numSamples)) recorded.fetch_add (numSamples, std::memory_order_relaxed);
            else                              dropped.fetch_add (1, std::memory_order_relaxed);
        }
        inWrite.store (0, std::memory_order_seq_cst);
    }

    // ================================ save / transcode ===============================

    // Locate an MP3 encoder. JUCE has no MP3 encoder of its own, so this is the whole MP3
    // story on BOTH platforms, and the search order is what makes it portable:
    //
    //   1. Beside the running binary (and a `bin`/`tools` subfolder). This is the Windows
    //      answer: drop lame.exe into the install folder and MP3 works with nothing on PATH.
    //   2. Every entry of PATH — the Linux answer (`apt install lame`, or ffmpeg, which
    //      most machines already carry).
    //
    // Both candidate names are tried with and without ".exe" on every platform, so there is
    // no per-OS name table to keep in sync. Returns a non-existent File when nothing is found;
    // callers surface installEncoderHint() rather than failing silently.
    static juce::File findMp3Encoder()
    {
        juce::Array<juce::File> dirs;
        auto exeDir = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
        dirs.add (exeDir);
        dirs.add (exeDir.getChildFile ("bin"));
        dirs.add (exeDir.getChildFile ("tools"));

        // The PATH separator is the one real per-OS difference here.
       #if JUCE_WINDOWS
        const char* pathSep = ";";
       #else
        const char* pathSep = ":";
       #endif
        juce::StringArray pathEntries;
        pathEntries.addTokens (juce::SystemStats::getEnvironmentVariable ("PATH", {}), pathSep, "");
        for (const auto& entry : pathEntries)
            if (entry.isNotEmpty())     // getChildFile passes an absolute entry straight through
                dirs.add (juce::File::getCurrentWorkingDirectory().getChildFile (entry));

        // `lame` first: a dedicated encoder, smaller and likelier to be the bundled one.
        for (const char* name : { "lame", "ffmpeg" })
            for (const auto& d : dirs)
            {
                if (! d.isDirectory()) continue;
                for (const auto& leaf : { juce::String (name), juce::String (name) + ".exe" })
                {
                    auto f = d.getChildFile (leaf);
                    if (f.existsAsFile()) return f;
                }
            }
        return {};
    }

    static bool mp3Available() { return findMp3Encoder().existsAsFile(); }

    // Shown in the dialog when MP3 is picked and no encoder exists. Names the fix per platform
    // instead of leaving the user with a dead menu entry.
    static juce::String installEncoderHint()
    {
       #if JUCE_WINDOWS
        return "MP3 needs an encoder: put lame.exe (or ffmpeg.exe) next to the app, or on your PATH.";
       #else
        return "MP3 needs an encoder: install lame (sudo apt install lame) or ffmpeg.";
       #endif
    }

    // Write the finished take to `dest` in `fmt`. WAV/FLAC/Ogg are re-encoded through JUCE
    // (portable, always present); MP3 shells out to the located encoder. Returns false and
    // fills `error` on any failure. Runs on the message thread (a save is not RT work).
    bool transcodeTo (const juce::File& dest, const Format& fmt, juce::String& error) const
    {
        if (! take.existsAsFile()) { error = "There is no recorded take to save."; return false; }

        if (fmt.kind == Kind::Mp3)
            return encodeMp3 (dest, fmt.param, error);

        juce::WavAudioFormat wavIn;
        std::unique_ptr<juce::AudioFormatReader> reader (wavIn.createReaderFor (take.createInputStream().release(), true));
        if (reader == nullptr) { error = "Could not read the recorded take."; return false; }

        std::unique_ptr<juce::AudioFormat> out;
        switch (fmt.kind)
        {
            case Kind::Wav:  out = std::make_unique<juce::WavAudioFormat>(); break;
           #if JUCE_USE_FLAC
            case Kind::Flac: out = std::make_unique<juce::FlacAudioFormat>(); break;
           #endif
           #if JUCE_USE_OGGVORBIS
            case Kind::Ogg:  out = std::make_unique<juce::OggVorbisAudioFormat>(); break;
           #endif
            default: break;
        }
        if (out == nullptr) { error = juce::String (fmt.label) + " is not supported in this build."; return false; }

        dest.deleteFile();
        auto os = dest.createOutputStream();
        if (os == nullptr) { error = "Could not create " + dest.getFullPathName(); return false; }

        // Ogg takes a quality-option INDEX, not a bit depth; WAV/FLAC take real bits.
        const int bits    = fmt.kind == Kind::Ogg ? 0 : fmt.param;
        const int quality = fmt.kind == Kind::Ogg ? juce::jlimit (0, out->getQualityOptions().size() - 1, fmt.param) : 0;
        std::unique_ptr<juce::AudioFormatWriter> w (
            out->createWriterFor (os.get(), reader->sampleRate, reader->numChannels, bits, {}, quality));
        if (w == nullptr) { error = "Could not encode " + juce::String (fmt.label) + "."; return false; }
        os.release();

        if (! w->writeFromAudioReader (*reader, 0, reader->lengthInSamples))
        { error = "Encoding failed part-way through."; return false; }
        w.reset();                                      // finalise headers before we report success
        return true;
    }

private:
    // MP3 via an external encoder. ChildProcess is handed an argv StringArray (never a
    // command line), so a path with spaces — "C:\Program Files\..." — needs no quoting and
    // no shell on either platform.
    bool encodeMp3 (const juce::File& dest, int kbps, juce::String& error) const
    {
        auto enc = findMp3Encoder();
        if (! enc.existsAsFile()) { error = installEncoderHint(); return false; }

        dest.deleteFile();
        const bool isFfmpeg = enc.getFileNameWithoutExtension().equalsIgnoreCase ("ffmpeg");
        juce::StringArray args;
        args.add (enc.getFullPathName());
        if (isFfmpeg)
            args.addArray ({ "-y", "-loglevel", "error", "-i", take.getFullPathName(),
                             "-codec:a", "libmp3lame", "-b:a", juce::String (kbps) + "k",
                             dest.getFullPathName() });
        else
            args.addArray ({ "--quiet", "-b", juce::String (kbps),
                             take.getFullPathName(), dest.getFullPathName() });

        juce::ChildProcess proc;
        if (! proc.start (args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        { error = "Could not run the MP3 encoder at " + enc.getFullPathName(); return false; }

        const auto out = proc.readAllProcessOutput();
        if (! proc.waitForProcessToFinish (600000)) { proc.kill(); error = "The MP3 encoder timed out."; return false; }
        if (proc.getExitCode() != 0 || ! dest.existsAsFile() || dest.getSize() == 0)
        {
            error = "MP3 encoding failed" + (out.isNotEmpty() ? ": " + out.trim().upToFirstOccurrenceOf ("\n", false, false)
                                                              : juce::String ("."));
            return false;
        }
        return true;
    }

    juce::TimeSliceThread thread { "synth-rec-writer" };
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
    // The audio thread reads `active` and publishes `inWrite`; stop() does the reverse. Both
    // seq_cst: this pair IS the teardown handshake, so relaxed orderings would let stop()
    // free the writer while the audio thread was still inside it.
    std::atomic<juce::AudioFormatWriter::ThreadedWriter*> active { nullptr };
    std::atomic<int> inWrite { 0 };
    std::atomic<juce::int64> recorded { 0 };
    std::atomic<int> dropped { 0 };
    juce::File take;
    double rate = 0.0;      // the rate the take was opened at (its WAV header); 0 = never started

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MasterRecorder)
};
