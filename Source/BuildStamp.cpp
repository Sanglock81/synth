// Synth — Copyright © 2026 John L Farmer. Licensed under AGPLv3; see LICENSE.
// Emits a uniquely-greppable build stamp into every VASynth binary (VST3 + Standalone) so the
// gate (run-all-checks.sh) can PROVE the artefact on disk was built from the gated commit.
// "Gate green" then means "the binaries ARE the gated code", not merely "an artefact exists".
//
// The stamp comes from VersionInfo.h, which cmake/gen_version.cmake regenerates on every build
// with the currently-built commit hash (+ a "+" suffix for an uncommitted tree). The symbol is
// otherwise unreferenced, so __attribute__((used)) keeps it past the linker's --gc-sections.
#include "VersionInfo.h"

// "used" stops the COMPILER discarding it; "retain" (GCC 11+/Clang, SHF_GNU_RETAIN) stops the
// LINKER's --gc-sections collecting the section. It is also referenced from PluginProcessor.cpp,
// so even a toolchain without "retain" keeps it — belt and suspenders.
#if defined(__has_attribute)
  #if __has_attribute(retain)
    #define VASYNTH_KEEP __attribute__((used, retain))
  #endif
#endif
#ifndef VASYNTH_KEEP
  #if defined(__GNUC__) || defined(__clang__)
    #define VASYNTH_KEEP __attribute__((used))
  #else
    #define VASYNTH_KEEP   // MSVC keeps extern non-static data by default
  #endif
#endif

extern "C" VASYNTH_KEEP const char vasynth_build_stamp[] = VASYNTH_BUILD_STAMP;
