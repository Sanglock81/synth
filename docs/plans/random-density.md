# RANDOM density reshape — design goal

**Keep full-range wildness; reshape the probability density toward usable.** The problem with
uniform Wild mode isn't randomness — it's that uniform sampling spends most of its probability mass
in regions no synthesist would dwell (a 22 s attack with a closed filter over a silent oscillator
isn't "wild," it's broken). Same support, radically better odds. Keep the existing
archetype / constrained / wild split; **all of this applies inside Wild.**

Five mechanisms, ordered by bang-per-effort:

1. **Perceptual distributions.** Sample times and frequencies **log-uniformly**, not uniformly (a
   5 ms vs 50 ms attack is a bigger musical difference than 20 s vs 20.05 s; uniform sampling
   doesn't know that). Same range, better odds. — cheapest, biggest win.
2. **Broken-patch invariants** (extend the audibility floor): filter not fully closed over the only
   sounding octave; attack + release not *both* extreme simultaneously; level staging normalized.
   These **cull defects, not character** — a screaming self-osc chaos patch passes; silence doesn't.
3. **A per-press coherence latent.** Roll one hidden "temperament" value per press
   (percussive ↔ sustained) and loosely correlate the envelopes to it. Random patches gain internal
   consistency — the quality that makes a sound feel intentional — while every parameter still varies.
4. **SEMI joins randomize quantized, not excluded.** (The shipped coarse-tune keeps it at 0.)
   Better: `{0, ±5, ±7, ±12}` with occasional chromatic outliers. Random intervals are a huge cheap
   win for interestingness. — supersedes the current `v = 0.5` (0 st) rule in `PresetManager::randomize`.
5. **Reject-and-resample** (the experimental one): render ~250 ms headlessly after generating; if it's
   silent, DC, or clipping-broken, silently re-roll (cap ~5 tries). This is the mechanism that lets
   everything else stay loose.

## Feasibility of (5) — the offline render check

**Cheap enough for a button press.** RANDOM fires on the message thread, not the audio thread, so a
few ms of latency is invisible. 250 ms @ 48 k = 12 000 samples ≈ 24 blocks of 512 → a scratch
`SynthEngine` setup + ~24 `render()` calls is well under ~2 ms; ≤5 attempts ≈ ≤10 ms worst case.
The **scratch-render path already exists** — `bakePresetParams()` renders candidate state into a
throwaway APVTS without disturbing the live voice, and the loudness/velocity tests render presets
headlessly the same way. So the work is: generate → bake to scratch → render 250 ms → classify
(silent / DC / clip-broken) → keep or re-roll — all off the audio thread. No new infrastructure.

## SEMI interval-variant audition batch (separate, for UAT Part 3.6)

Curated listening pass, **not a retro-edit sweep**. Intervals transform specific patch *classes*;
propose ~8 candidates as auditionable variant presets (new names, originals untouched); only winners
commit. `Fifth Stack` already proves the concept. See the H-follow-on task.
