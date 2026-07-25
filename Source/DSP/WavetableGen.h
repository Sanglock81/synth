#pragma once
#include "Wavetable.h"
#include <cstdint>

// ============================================================================
// #95 Wavetable content — the DETERMINISTIC generator behind the factory tables
// and the seeded randomizer. JUCE-free.
//
// Everything a table is built from must regenerate BIT-IDENTICALLY on every
// platform, because a randomized table is persisted by SEED (not by bytes): a
// preset made on Linux and reopened on Windows must produce the same table, or it
// is a silent content fork. To guarantee that, the implementation (WavetableGen.cpp)
//   * owns its PRNG (a fixed xorshift — never a std distribution), and
//   * uses only +,-,*,/ and a contraction-free polynomial sine (no std::sin / pow
//     / exp), compiled with FP contraction OFF so no platform fuses a mul-add.
// A committed byte-exact golden (checked in CI on Windows) proves it.
//
// Factory tables and the randomizer share ONE build+normalize path (equal-RMS), so
// switching tables or re-rolling the die never jumps the level.
// ============================================================================

namespace wtgen
{
    constexpr int kFrameLen = 256;      // samples per frame (power of two)

    int         factoryCount();                                  // number of built-in tables
    const char* factoryName (int id);                            // display name (ASCII)
    Wavetable   buildFactory (int id, double sampleRate);        // id in [0, factoryCount())
    Wavetable   buildRandom  (std::uint32_t seed, double sampleRate);   // seeded, repeatable
}
