# #95 — Wavetable oscillator (the 5th wave option, "WT")

> Committed plan so the next session starts from the repo, not from memory. Approved as written
> (3a/3b/3c, gates between each). Persist randomized tables **by seed** (confirmed). All work
> ships off-by-default so goldens stay bit-identical.

## Context (verified against master @ this commit)

- **Oscillator** = `Source/DSP/PolyBlepOscillator.h`. `enum class Wave { Saw, Square, Triangle, Sine }`
  — **append `Wavetable` only, never reorder** (choice values are frozen for state compat). Saw/Square/
  Triangle run at `osRate` (4× oversample) through a windowed-sinc **decimation FIR**; **Sine bypasses**
  and runs at base rate. Quality modes: None (1×), Efficient (4× + 48-tap, DEFAULT), HQ (4× + 320-tap).
- **The WavePos seam is already fully reserved** (do NOT re-add): `ModMatrix::Dest::WavePos` (voice-tier),
  `Offsets.wavePos`, `kRangeWavePos = 1.0f`, `evaluate()` already does `o.wavePos += v * kRangeWavePos`,
  and `ModDestRegistry.h` has `{ ModMatrix::WavePos, "", "Wave Pos", Osc }` (empty paramId = overlay-only,
  no knob yet). The voice just needs to **consume `mm.wavePos`**; add the base `oscN_wt_pos` params it sums with.
- Per-osc choice params are `osc1Wave/osc2Wave/osc3Wave` (Pc, `waveNames { "Saw","Square","Triangle","Sine" }`
  in `Parameters.h` ~line 228) — append `"WT"`. Wave→voice plumbing: `VoiceParams.oscNWave` (int) → osc `setWave`.
- Factory content is embedded via BinaryData (see `SampleStore`/kit content for the pattern); `AppInfo` has
  per-user dirs. Presets/MULTIs round-trip through the APVTS state tree + custom trees.
- Aliasing "soul" test already exists in `tests/dsp/test_filter.cpp` (full radix-2 FFT, harmonic vs
  non-harmonic bin energy). Reuse that FFT harness for the WT aliasing test.

## 3a — Engine (DSP)  ← start here next session

- New `Source/DSP/Wavetable.h` (JUCE-free, std-only, allocation-free after build):
  - A table = **N frames** (e.g. 64 or 256 samples/frame; a handful of frames for position morph).
  - **Mip-mapped band-limiting**: at build (message thread, in prepare/load — NOT RT), for each frame
    build **per-octave** band-limited copies: forward FFT of the frame → zero all bins above that octave's
    harmonic limit (so the top mip has ~1 harmonic, each lower octave doubles the harmonics) → IFFT →
    store. Mip count ~ log2(tableLen). Normalize (see loudness rule in 3b).
  - **Runtime read** (called at `osRate` inside `coreSample()` like saw/square): pick the **mip by pitch**
    (fundamental Hz → highest mip whose top harmonic ≤ base Nyquist), **linear-interpolate within the frame**,
    and **crossfade between the two adjacent frames** by the position `pos∈[0,1]` (`floor(pos*(N-1))` and its
    neighbour). Two table reads + interp, per the spec.
- `PolyBlepOscillator`: append `Wave::Wavetable`; give the osc a `const Wavetable*` (or an index into a shared
  bank) + a `setWavePosition(double)`; in `coreSample()` add the `case Wave::Wavetable` that reads the table.
  Keep it inside the oversample/decimate path (the FIR mops up any residual). `nextSample()` Sine-style bypass
  is NOT used for WT.
- `SynthVoice`: `VoiceParams` gains `float wtPos` (base position); the voice computes
  `clamp(p.wtPos + mm.wavePos, 0, 1)` and **smooths it** (zipper-safe) before `setWavePosition`. Wire the
  table selection (which table this osc uses) through too.
- Params (`Parameters.h`): `osc1_wt_pos / osc2_wt_pos / osc3_wt_pos` (0..1, default 0). Append `"WT"` to
  `waveNames`. A per-osc **table index** needs storage too (see 3b — likely a string/id param or a custom
  state field, since a seed-backed randomized table isn't a fixed choice list).
- **Tests (3a)**: (1) **aliasing soul test extended over WT** — high notes (e.g. C7/C8) + position sweeps,
  asserting the non-harmonic energy stays below the same floor the other waves meet (mip correctness is what's
  under test). (2) **position-crossfade continuity** — sweeping `wt_pos` produces no sample step above the
  click threshold and no discontinuity between adjacent frames. (3) **goldens bit-identical** (WT is not the
  default wave, so default render is unchanged) — the standing golden gate. (4) zipper test on `wt_pos`.

## 3b — Content (factory tables + seeded randomizer)

- **3–4 factory tables**, embedded like other factory content: analog-morph (saw→square-ish sweep),
  formant/vowel (a→e→i→o→u spectral peaks), bright-digital (harsh additive), harmonic-sweep (fundamental →
  rich). Build their mips through the **same** path as the randomizer.
- **Randomizer**: one tap generates a **seeded, repeatable** table with a **bounded harmonic count** (random
  amplitudes/phases over ≤K harmonics), bakes its mips, and selects it. Factory tables + randomizer output use
  the **identical build + normalization path**.
- **Loudness normalization rule (state it + test it)**: normalize every table (factory + random) to **equal
  RMS** (or equal peak — pick RMS for perceptual parity) *after* mip band-limiting, so switching tables /
  re-rolling the die does not jump the level. Test: the RMS of a rendered note is within ±X% across all
  factory tables and several random seeds.
- **Persistence — by SEED** (confirmed): a randomized table persists as its **seed + K** (compact,
  deterministic, matches "seeded, repeatable"); factory tables persist by **id/index**. On load, re-generate
  the table from the seed (identical bytes → identical mips). Round-trip test: save a preset/MULTI with a
  randomized WT osc, reload in a fresh instance, assert the rendered output matches bit-for-bit (seed
  determinism) and the table choice/position survive.

## 3c — UI (the user's LOCKED interaction — build exactly this)

- The osc wave row (`HSelector` SIN..) gains the **WT** option (append).
- **TAP on WT when it is already the selected wave** → opens the **table picker** (choose factory table or the
  current random one).
- **TOUCH-AND-HOLD on WT** → opens the **preset-table picker directly** (the factory list).
- The **randomizer** is a **die / refresh affordance beside the picker** (one tap re-rolls a seeded table).
- A compact **WT POS** knob appears **only for a WT-mode osc** (hidden otherwise) — use the **SYNC/DIV
  visibility idiom** (`ParameterAttachment` flips visibility; same-bounds morph) already used for the LFO
  RATE↔DIV knob and the EnvSection AMP/MOD swap.
- **Screenshot sign-off** (`docs/smoke/` + the fullscreen `docs/editor.png`).
- The **full drawable WT editor stays post-1.0** (roadmap) — this is selection + position only.
- **Factory preset touch-up rides along**: 2–3 presets showcasing WT (evolving pad on a position-LFO,
  formant lead, the doc's character settings — RANDOM start-phase + a breath of ANALOG where they suit).

## Verification (whole feature)

- `run-all-checks.sh` (Release + pluginval 8) and `--sanitize` green per sub-increment; **goldens
  bit-identical** (WT not default).
- **Bench**: a WT voice vs a saw voice, ThinkPad-derated, reported (add a `measure` scenario in
  `tests/bench/bench_engine.cpp`). Expect a modest add (two table reads + interp inside the existing path).
- Hands-on: select WT on an osc → tap opens the picker → the die re-rolls → WT POS morphs the timbre; a
  position-LFO makes it evolve; high notes stay clean (no aliasing).

## Standing items carried into the next session

- **Drive-character verdict** (Tier 2 `kMaxDriveGain`, currently 4.0) may arrive from the user as a one-line
  constant tune — apply when it lands.
- **#100 ThinkPad deployment validation** remains the pre-tag gate (must include the driven-voices xrun
  stress scenario per the Option-1 decision). **#102** WILD-randomize-into-self-osc is a post-2C follow-up.
- Roadmap after #95: **#96 unison** (delivers Musicality Tier 3; pre-agreed default-off / Efficient-cap
  contingency) → **#97 QWERTY v2** → **#98 session export** → **#100 ThinkPad validation** → **#101 tag v1.0.0**.
