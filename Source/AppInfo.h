#pragma once
#include <juce_core/juce_core.h>

// ============================================================================
// Single source of truth for the app's user-facing identity and its on-disk
// config location, plus a one-time migration from the pre-rename "VASynth"
// locations to the new "synth" ones so an existing rig keeps its presets, MIDI
// profiles, and (standalone) audio/MIDI settings after the rename.
//
// The plugin's display name (VST3 filename, window title) comes from
// PRODUCT_NAME in CMake; keep it in sync with kName here. Config subdirectories
// are chosen explicitly (not derived from the plugin name) so they are stable
// and testable across the plugin and standalone.
// ============================================================================

namespace AppInfo
{
    inline constexpr const char* kName       = "synth";     // user-facing name
    inline constexpr const char* kLegacyName = "VASynth";   // pre-rename config dir

    // Base user-data directory (Linux: ~/.config, Windows: AppData/Roaming, etc.).
    inline juce::File dataRoot()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
    }

    inline juce::File configDir()       { return dataRoot().getChildFile (kName); }
    inline juce::File legacyConfigDir() { return dataRoot().getChildFile (kLegacyName); }

    inline juce::File presetDir()      { auto d = configDir().getChildFile ("presets");       d.createDirectory(); return d; }
    inline juce::File midiProfileDir() { auto d = configDir().getChildFile ("midi-profiles"); d.createDirectory(); return d; }
    inline juce::File multiDir()       { auto d = configDir().getChildFile ("multis");        d.createDirectory(); return d; }
    inline juce::File kitDir()         { auto d = configDir().getChildFile ("kits");          d.createDirectory(); return d; }
    inline juce::File logFile()        { return configDir().getChildFile (juce::String (kName) + ".log"); }

    // Core migration (pure, so it is unit-testable with temp dirs): copy each of
    // the config SUBDIRS from `legacy` to `dest`, but only when the destination
    // subdir does not yet exist. Per-subdir + existence-gated makes it idempotent
    // and not blocked by unrelated files (e.g. a freshly-created log) already in
    // `dest`. Returns the number of subdirs actually copied.
    inline int migrateConfigTree (const juce::File& legacy, const juce::File& dest)
    {
        if (! legacy.isDirectory()) return 0;
        dest.createDirectory();
        int copied = 0;
        for (const char* sub : { "presets", "midi-profiles" })
        {
            auto from = legacy.getChildFile (sub);
            auto to   = dest.getChildFile (sub);
            if (from.isDirectory() && ! to.exists())
            {
                to.createDirectory();
                if (from.copyDirectoryTo (to)) ++copied;   // copies the user's saved files
            }
        }
        return copied;
    }

    // One-time migration of the real legacy config. Safe on every launch: a no-op
    // once migrated or when there is no legacy data (fresh machine / CI).
    inline void migrateLegacyConfig() { migrateConfigTree (legacyConfigDir(), configDir()); }

    // Standalone-only: migrate the JUCE PropertiesFile (audio/MIDI settings) from
    // the old "<legacy>.settings" to "<name>.settings" in the same folder, once.
    // `settingsFolder` is where the standalone stores its .settings file.
    inline void migrateLegacySettings (const juce::File& settingsFolder)
    {
        auto to   = settingsFolder.getChildFile (juce::String (kName) + ".settings");
        if (to.existsAsFile()) return;
        // Pre-rename the standalone used the product name "VA Synth".
        auto from = settingsFolder.getChildFile ("VA Synth.settings");
        if (from.existsAsFile())
            from.copyFileTo (to);
    }
}
