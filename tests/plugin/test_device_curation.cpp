// ============================================================================
// Bug 1: audio-output device-list curation. Pure StringArray policy — no device
// is opened — so a realistic PipeWire+ALSA "wall" can be exercised headlessly.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "Standalone/AudioDeviceCuration.h"

namespace
{
    // A representative ThinkPad list: the friendly PipeWire endpoint, a couple of
    // real cards, and the usual pile of raw ALSA aliases/duplicates.
    juce::StringArray thinkpadWall()
    {
        return {
            "Default ALSA Output (currently PipeWire Media Server)",
            "pipewire",
            "pulse",
            "default",
            "Scarlett 2i2 USB",
            "HDA Intel PCH",
            "hw:0,0",
            "hw:1,0",
            "plughw:0,0",
            "plughw:1,0",
            "front:CARD=PCH,DEV=0",
            "surround40:CARD=PCH,DEV=0",
            "surround51:CARD=PCH,DEV=0",
            "hdmi:CARD=HDMI,DEV=0",
            "dmix:CARD=PCH,DEV=0",
            "dsnoop:CARD=PCH,DEV=0",
            "sysdefault:CARD=PCH"
        };
    }
}

TEST_CASE ("curation prefers the PipeWire/default endpoint as the default device", "[plugin][bug1][curation]")
{
    using namespace AudioDeviceCuration;
    const auto chosen = pickPreferredDeviceName (thinkpadWall());
    INFO ("picked: " << chosen);
    REQUIRE (chosen.containsIgnoreCase ("Default ALSA Output"));   // the OS-default-following endpoint
}

TEST_CASE ("curation falls back to a friendly card name when no endpoint exists", "[plugin][bug1][curation]")
{
    using namespace AudioDeviceCuration;
    juce::StringArray noEndpoint { "hw:0,0", "plughw:0,0", "Scarlett 2i2 USB", "front:CARD=PCH,DEV=0" };
    const auto chosen = pickPreferredDeviceName (noEndpoint);
    REQUIRE (chosen == "Scarlett 2i2 USB");        // the one non-alias, friendly name
}

TEST_CASE ("curated list drops raw ALSA aliases but keeps endpoints + card names", "[plugin][bug1][curation]")
{
    using namespace AudioDeviceCuration;
    const auto curated = curateDeviceList (thinkpadWall());
    INFO ("curated: " << curated.joinIntoString (" | "));

    // Kept: the friendly endpoints and the real card names.
    REQUIRE (curated.contains ("Default ALSA Output (currently PipeWire Media Server)"));
    REQUIRE (curated.contains ("pipewire"));
    REQUIRE (curated.contains ("Scarlett 2i2 USB"));
    REQUIRE (curated.contains ("HDA Intel PCH"));

    // Dropped: every raw alias / duplicate the user complained about.
    for (const auto& alias : { "hw:0,0", "hw:1,0", "plughw:0,0", "plughw:1,0",
                               "front:CARD=PCH,DEV=0", "surround40:CARD=PCH,DEV=0",
                               "surround51:CARD=PCH,DEV=0", "hdmi:CARD=HDMI,DEV=0",
                               "dmix:CARD=PCH,DEV=0", "dsnoop:CARD=PCH,DEV=0",
                               "sysdefault:CARD=PCH" })
        REQUIRE_FALSE (curated.contains (alias));

    REQUIRE (curated.size() < thinkpadWall().size());     // genuinely shorter
}

TEST_CASE ("showAll escape hatch returns the full raw list unchanged", "[plugin][bug1][curation]")
{
    using namespace AudioDeviceCuration;
    const auto all = curateDeviceList (thinkpadWall(), /*showAll=*/true);
    REQUIRE (all == thinkpadWall());
}

TEST_CASE ("curation never hides everything (all-alias list survives)", "[plugin][bug1][curation]")
{
    using namespace AudioDeviceCuration;
    juce::StringArray allAliases { "hw:0,0", "plughw:0,0", "dmix:CARD=PCH,DEV=0" };
    const auto curated = curateDeviceList (allAliases);
    REQUIRE_FALSE (curated.isEmpty());             // better a raw list than no list
    REQUIRE (curated == allAliases);
}

TEST_CASE ("curation handles an empty list without crashing", "[plugin][bug1][curation]")
{
    using namespace AudioDeviceCuration;
    REQUIRE (pickPreferredDeviceName ({}).isEmpty());
    REQUIRE (curateDeviceList ({}).isEmpty());
}
