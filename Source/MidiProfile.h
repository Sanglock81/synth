#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <vector>

// ============================================================================
// MIDI device profiles: a small JSON schema mapping a controller to sensible
// default CC->parameter bindings, so a known device (Launchkey Mini, Korg B2)
// works the moment it's plugged in — no manual MIDI-learn required.
//
//   { "name": "...", "match": ["substr", ...], "pitchBendRange": 2,
//     "mappings": [ { "cc": 21, "param": "filter_cutoff" }, ... ] }
//
// A profile matches a device when any `match` string is a (case-insensitive)
// substring of the device name. Profiles come from two places:
//   * FACTORY  — embedded in the binary (BinaryData), added via addFactory().
//   * USER     — JSON files in the user config dir, added via addUser().
// Precedence when applied is learned > user > factory (enforced by the
// MidiLearnManager source tags); the library just resolves which profiles match.
//
// Pure/JUCE-JSON only and BinaryData-free, so the parser is unit-testable with
// literal strings; the binary embedding is wired separately.
// ============================================================================

struct MidiProfile
{
    juce::String            name;
    juce::StringArray       match;
    int                     pitchBendRange = 2;
    std::vector<std::pair<int, juce::String>> mappings;   // (cc, paramID)
    bool                    isUser = false;

    bool matchesDevice (const juce::String& deviceName) const
    {
        const auto dev = deviceName.toLowerCase();
        for (const auto& m : match)
            if (m.isNotEmpty() && dev.contains (m.toLowerCase()))
                return true;
        return false;
    }

    // Parse one profile object. Returns false (and leaves *this partly filled) on
    // malformed input; callers should check ok before using it.
    static MidiProfile fromJson (const juce::String& json, bool& ok)
    {
        MidiProfile p;
        ok = false;
        auto v = juce::JSON::parse (json);
        if (! v.isObject()) return p;

        p.name = v.getProperty ("name", juce::String()).toString();
        if (auto* matchArr = v.getProperty ("match", juce::var()).getArray())
            for (auto& m : *matchArr) p.match.add (m.toString());
        p.pitchBendRange = (int) v.getProperty ("pitchBendRange", 2);

        if (auto* maps = v.getProperty ("mappings", juce::var()).getArray())
        {
            for (auto& m : *maps)
            {
                if (! m.isObject()) continue;
                const int cc = (int) m.getProperty ("cc", -1);
                const auto param = m.getProperty ("param", juce::String()).toString();
                if (cc >= 0 && cc < 128 && param.isNotEmpty())
                    p.mappings.emplace_back (cc, param);
            }
        }
        // A profile is valid if it at least names itself and has a match rule.
        ok = p.name.isNotEmpty() && ! p.match.isEmpty();
        return p;
    }
};

class MidiProfileLibrary
{
public:
    void addFactory (const juce::String& json)
    {
        bool ok = false;
        auto p = MidiProfile::fromJson (json, ok);
        if (ok) { p.isUser = false; profiles.push_back (std::move (p)); }
    }

    void addUser (const juce::String& json)
    {
        bool ok = false;
        auto p = MidiProfile::fromJson (json, ok);
        if (ok) { p.isUser = true; profiles.push_back (std::move (p)); }
    }

    // Load every *.json in a user directory (if it exists) as a user profile.
    void loadUserDir (const juce::File& dir)
    {
        if (! dir.isDirectory()) return;
        for (auto& f : dir.findChildFiles (juce::File::findFiles, false, "*.json"))
            addUser (f.loadFileAsString());
    }

    // The matching factory / user profile for a device (last one wins so a later
    // addition overrides an earlier). Null if none matches.
    const MidiProfile* factoryFor (const juce::String& deviceName) const { return find (deviceName, false); }
    const MidiProfile* userFor    (const juce::String& deviceName) const { return find (deviceName, true); }

    int size() const { return (int) profiles.size(); }

private:
    const MidiProfile* find (const juce::String& deviceName, bool wantUser) const
    {
        const MidiProfile* result = nullptr;
        for (const auto& p : profiles)
            if (p.isUser == wantUser && p.matchesDevice (deviceName))
                result = &p;                       // last match wins
        return result;
    }

    std::vector<MidiProfile> profiles;
};
