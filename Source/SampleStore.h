#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include "AppInfo.h"
#include <vector>
#include <memory>
#include <utility>

// ============================================================================
// I2 — the managed, content-deduplicated sample library (message thread only).
//
// A drum-kit pad references a sample by its CONTENT HASH (md5). importFile() copies the
// WAV/AIFF/FLAC into ~/.config/synth/samples/<md5>.<ext> (once — five kits loading the same
// file share one on-disk copy) and decodes it into a resident stereo buffer. resolve()
// returns the loaded buffer for a key, loading from the managed dir on demand.
//
// LIFETIME CONTRACT (the audio thread borrows raw pointers into these buffers): a SampleData
// is IMMUTABLE after load and is NEVER freed for the process lifetime; its inner vectors are
// sized once and never resized. The memory cap REFUSES further imports (logged) — it never
// evicts a loaded buffer. So a SampleData* handed to a voice is valid forever.
// ============================================================================

struct SampleData
{
    std::vector<float> L, R;          // decoded, native-rate stereo (mono files duplicated)
    int    len      = 0;
    double nativeSR = 48000.0;
};

class SampleStore
{
public:
    SampleStore() { formats.registerBasicFormats(); }   // WAV / AIFF / FLAC / OGG ...

    // Import an audio file: hash its bytes, copy into the managed dir if absent (dedup), load.
    // Returns the md5 key, or empty on failure / cap refusal.
    juce::String importFile (const juce::File& src)
    {
        if (! src.existsAsFile()) return {};
        juce::MemoryBlock bytes;
        if (! src.loadFileAsData (bytes) || bytes.getSize() == 0) return {};

        const juce::String key = contentHash (bytes);
        const juce::String ext = src.getFileExtension().toLowerCase();       // ".wav" / ".aiff" / ".flac"
        const juce::File managed = AppInfo::samplesDir().getChildFile (key + ext);
        if (! managed.existsAsFile()) managed.replaceWithData (bytes.getData(), bytes.getSize());
        return ensureLoaded (key) ? key : juce::String();
    }

    // The resident buffer for a key (loads from the managed dir if needed). Null if missing/refused.
    // The returned pointer is STABLE for the process lifetime (never freed / never moved).
    const SampleData* resolve (const juce::String& key)
    {
        if (key.isEmpty()) return nullptr;
        if (auto* p = find (key)) return p;
        return ensureLoaded (key) ? find (key) : nullptr;
    }

    int loadedCount() const { return (int) pool.size(); }

private:
    // 64-bit FNV-1a over the file bytes + length, as a hex string. Dependency-free (no
    // juce_cryptography); collision risk is negligible for content-addressed sample dedup.
    static juce::String contentHash (const juce::MemoryBlock& b)
    {
        std::uint64_t h = 1469598103934665603ull;                 // FNV offset basis
        const auto* p = static_cast<const std::uint8_t*> (b.getData());
        for (size_t i = 0; i < b.getSize(); ++i) { h ^= p[i]; h *= 1099511628211ull; }
        h ^= (std::uint64_t) b.getSize();
        return juce::String::toHexString ((juce::int64) h).replace (" ", "");
    }

    SampleData* find (const juce::String& key) const
    {
        for (auto& e : pool) if (e.first == key) return e.second.get();
        return nullptr;
    }

    bool ensureLoaded (const juce::String& key)
    {
        if (find (key) != nullptr) return true;

        juce::File f;
        for (auto& c : AppInfo::samplesDir().findChildFiles (juce::File::findFiles, false, key + ".*"))
            { f = c; break; }
        if (! f.existsAsFile()) return false;

        // Never evict: once the budget is reached, refuse new loads (logged) instead of freeing.
        if (loadedSeconds >= kMaxSampleSeconds)
        { juce::Logger::writeToLog ("SampleStore: " + juce::String (kMaxSampleSeconds, 0)
                                    + "s cap reached, refusing " + key); return false; }

        std::unique_ptr<juce::AudioFormatReader> rd (formats.createReaderFor (f));
        if (rd == nullptr || rd->lengthInSamples <= 0) return false;

        const int n  = (int) juce::jmin (rd->lengthInSamples, (juce::int64) (kMaxSampleSeconds * rd->sampleRate));
        const int ch = (int) juce::jmax (1u, rd->numChannels);
        juce::AudioBuffer<float> buf (ch, n);
        rd->read (&buf, 0, n, 0, true, ch > 1);

        auto sd = std::make_unique<SampleData>();
        sd->len = n; sd->nativeSR = rd->sampleRate > 0 ? rd->sampleRate : 48000.0;
        sd->L.assign ((std::size_t) n, 0.0f);
        sd->R.assign ((std::size_t) n, 0.0f);
        const float* l = buf.getReadPointer (0);
        const float* r = ch > 1 ? buf.getReadPointer (1) : l;                // mono -> duplicate to both
        for (int i = 0; i < n; ++i) { sd->L[(std::size_t) i] = l[i]; sd->R[(std::size_t) i] = r[i]; }

        loadedSeconds += (double) n / sd->nativeSR;
        pool.emplace_back (key, std::move (sd));                             // append-only, never removed
        return true;
    }

    // ~600 s of resident stereo @48k ~= 230 MB ceiling. Raise this one constant for a studio build.
    static constexpr double kMaxSampleSeconds = 600.0;

    juce::AudioFormatManager formats;
    std::vector<std::pair<juce::String, std::unique_ptr<SampleData>>> pool;   // md5 -> data (stable, never freed)
    double loadedSeconds = 0.0;
};
