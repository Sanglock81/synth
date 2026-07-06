#pragma once
#include <juce_core/juce_core.h>

// ============================================================================
// Audio-output device-list curation (Bug 1).
//
// On a typical Linux/PipeWire box JUCE's ALSA enumeration is a wall of aliases —
// hw:/plughw:/front:/surround:/hdmi:/dmix:/dsnoop: duplicates of the same few
// physical cards — and it is not obvious which one actually makes sound. These
// pure helpers (StringArray in/out, no hardware) implement the policy:
//
//   * pickPreferredDeviceName() — the "just works" default: prefer the
//     PipeWire/default endpoint, which follows the OS default sink and therefore
//     routes to whatever the user selected there (including the Scarlett 2i2).
//   * curateDeviceList()        — hide the redundant raw ALSA aliases, keeping
//     the friendly endpoints and per-card names. `showAll` is the escape hatch.
//
// Kept JUCE-free of anything but juce_core (StringArray) so they unit-test
// without opening a device.
// ============================================================================

namespace AudioDeviceCuration
{
    // Prefixes of the raw ALSA PCM aliases we hide from the curated list. These
    // are the "wall" — plumbing PCMs, not things a player should have to choose
    // between. Matched case-insensitively at the START of the device name.
    inline const juce::StringArray& rawAliasPrefixes()
    {
        static const juce::StringArray prefixes {
            "hw:", "plughw:", "front:", "surround", "rear:", "center_lfe",
            "side:", "hdmi:", "dmix:", "dsnoop:", "iec958:", "modem:",
            "phoneline:", "sysdefault:card", "dessert:", "usbstream:"
        };
        return prefixes;
    }

    // The friendly "default" endpoints, in preference order. The first one present
    // in the device list is the sensible default to open.
    inline const juce::StringArray& preferredEndpoints()
    {
        static const juce::StringArray prefer {
            "Default ALSA Output", "pipewire", "PipeWire", "pulse", "default"
        };
        return prefer;
    }

    inline bool isEndpoint (const juce::String& name)
    {
        const auto n = name.toLowerCase();
        return n.contains ("pipewire") || n.contains ("pulse")
            || n.startsWith ("default") || n.contains ("default alsa output");
    }

    inline bool isRawAlias (const juce::String& name)
    {
        const auto n = name.toLowerCase();
        for (const auto& p : rawAliasPrefixes())
            if (n.startsWith (p.toLowerCase()))
                return true;
        return false;
    }

    // The "just works" default output device name, or empty if `available` is
    // empty. Prefers the PipeWire/default endpoint, then any friendly (non-alias)
    // name, then — only if nothing else — the first raw entry.
    inline juce::String pickPreferredDeviceName (const juce::StringArray& available)
    {
        if (available.isEmpty()) return {};

        for (const auto& pref : preferredEndpoints())
            for (const auto& n : available)
                if (n.containsIgnoreCase (pref))
                    return n;

        for (const auto& n : available)          // first friendly (non-alias) name
            if (! isRawAlias (n))
                return n;

        return available[0];                     // everything is an alias; take one
    }

    // The curated device list: friendly endpoints + non-alias names, with the raw
    // aliases removed. `showAll` returns the full list unchanged (the advanced
    // escape hatch). Never returns empty when the input was non-empty — if every
    // entry looked like an alias, the raw list is returned so the user is never
    // left with nothing to pick.
    inline juce::StringArray curateDeviceList (const juce::StringArray& available, bool showAll = false)
    {
        if (showAll || available.isEmpty()) return available;

        juce::StringArray out;
        for (const auto& n : available)
            if (isEndpoint (n) || ! isRawAlias (n))
                out.addIfNotAlreadyThere (n);

        return out.isEmpty() ? available : out;
    }
}
