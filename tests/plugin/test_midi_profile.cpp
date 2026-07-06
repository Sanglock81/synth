// ============================================================================
// [6c] MIDI device profile parsing, device matching, and factory/user resolution.
// (The precedence-on-apply behaviour is tested against MidiLearnManager in
// test_midi_learn.cpp.)
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include "MidiProfile.h"

namespace
{
    const char* kLaunchkey =
        R"({ "name":"Launchkey Mini", "match":["Launchkey Mini","LKMK3 Mini"],
             "pitchBendRange":2,
             "mappings":[ {"cc":21,"param":"filter_cutoff"}, {"cc":22,"param":"filter_reso"} ] })";

    const char* kEmptyMap =
        R"({ "name":"Korg B2", "match":["Korg B2","B2"], "mappings":[] })";
}

TEST_CASE ("MidiProfile parses a well-formed profile", "[plugin][6c][profile]")
{
    bool ok = false;
    auto p = MidiProfile::fromJson (kLaunchkey, ok);
    REQUIRE (ok);
    REQUIRE (p.name == "Launchkey Mini");
    REQUIRE (p.pitchBendRange == 2);
    REQUIRE (p.mappings.size() == 2);
    REQUIRE (p.mappings[0].first == 21);
    REQUIRE (p.mappings[0].second == "filter_cutoff");
}

TEST_CASE ("MidiProfile device matching is case-insensitive substring", "[plugin][6c][profile]")
{
    bool ok = false;
    auto p = MidiProfile::fromJson (kLaunchkey, ok);
    REQUIRE (ok);
    REQUIRE (p.matchesDevice ("Launchkey Mini MK3 MIDI"));   // substring + extra text
    REQUIRE (p.matchesDevice ("novation launchkey mini"));   // case-insensitive
    REQUIRE_FALSE (p.matchesDevice ("Korg B2"));
}

TEST_CASE ("MidiProfile tolerates an empty mapping list and rejects junk", "[plugin][6c][profile]")
{
    bool ok = false;
    auto empty = MidiProfile::fromJson (kEmptyMap, ok);
    REQUIRE (ok);
    REQUIRE (empty.mappings.empty());

    bool ok2 = true;
    MidiProfile::fromJson ("not json at all", ok2);
    REQUIRE_FALSE (ok2);

    bool ok3 = true;
    MidiProfile::fromJson (R"({ "mappings":[] })", ok3);     // no name/match
    REQUIRE_FALSE (ok3);
}

TEST_CASE ("MidiProfileLibrary resolves factory vs user by device, user separate", "[plugin][6c][profile]")
{
    MidiProfileLibrary lib;
    lib.addFactory (kLaunchkey);
    lib.addFactory (kEmptyMap);
    REQUIRE (lib.size() == 2);

    const auto* fac = lib.factoryFor ("Launchkey Mini MK3");
    REQUIRE (fac != nullptr);
    REQUIRE (fac->name == "Launchkey Mini");
    REQUIRE (lib.userFor ("Launchkey Mini MK3") == nullptr);      // no user profile yet

    // A user profile for the same device is resolved independently of factory.
    lib.addUser (R"({ "name":"My Launchkey", "match":["Launchkey Mini"],
                      "mappings":[ {"cc":21,"param":"reverb_mix"} ] })");
    const auto* usr = lib.userFor ("Launchkey Mini MK3");
    REQUIRE (usr != nullptr);
    REQUIRE (usr->mappings[0].second == "reverb_mix");
    REQUIRE (lib.factoryFor ("Launchkey Mini MK3") != nullptr);   // factory still present
}
