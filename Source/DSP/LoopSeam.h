#pragma once

// ============================================================================
// #146 Bug A — MIDI loop-seam declick. A note held to the loop end has its synthesized
// note-off (Looper::closeDangling) nudged this many samples EARLY, so its release envelope
// doesn't collide with the next pass's re-attack at the wrap (the seam click the user heard
// as "a note playing when the loop ends has a hard stop"). The note's own release does the
// fade — no new fade path. Inaudible on a slow-release pad; softens a fast pluck's transient.
// ~5 ms @ 48 kHz.
//
// (The AUDIO-lane seam is the same class but needs a tempo-safe crossfade — a naive edge-window
// dips audibly on sustained material and alters playback fidelity — so it is deferred to a
// dedicated pass. Named here as one shared constant for when that lands.)
// ============================================================================

namespace loopseam
{
    constexpr int kSeamSamples = 240;
}
