// Synth — Copyright © 2026 John L Farmer. Licensed under AGPLv3; see LICENSE.
#pragma once

// ============================================================================
// #146 Bug A — MIDI loop-seam declick. A note held to the loop end has its synthesized
// note-off (Looper::closeDangling) nudged this many samples EARLY, so its release envelope
// doesn't collide with the next pass's re-attack at the wrap (the seam click the user heard
// as "a note playing when the loop ends has a hard stop"). The note's own release does the
// fade — no new fade path. Inaudible on a slow-release pad; softens a fast pluck's transient.
// ~5 ms @ 48 kHz.
//
// AUDIO lane (AudioLoop::playBlock): over the last kSeamSamples the loop tail is ramped toward the
// head's first sample, so the circular-buffer wrap is continuous with no step. Loop length is
// preserved exactly (tempo locked); the ramp approaches a real sample value, not silence, so
// steady/sustained material passes through transparently (no dip) while any start/end mismatch is
// declicked. One shared constant, one ramp to trust.
// ============================================================================

namespace loopseam
{
    constexpr int kSeamSamples = 240;
}
