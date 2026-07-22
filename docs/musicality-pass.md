# VA Synth — Sound Quality Improvement Package ("Musicality Pass")

> Governing spec for task #99. Build to this document exactly — its specifics govern
> (phase policy modes, drive=0 fast path proven zero-cost, fast-tanh with tolerance test,
> the oversampling evaluation, self-oscillation behavior). Restored to the repo 2026-07 after
> it was lost to a compacted session.
>
> **STATUS — COMPLETE (#99).** Tier 1 (start-phase policy + analog drift) ✓ · Tier 2 (nonlinear
> filter: linear fast path, in-loop tanh drive, self-oscillation + keytrack, 2× oversampling) ✓ ·
> Tier 4a (modulated reverb tail — MOTION on the allpass diffusers) ✓ · Tier 4c (dual-tap chorus —
> VOICES 1|2) ✓. **Tier 3 (unison character) is delivered by the unison work item (#96), as agreed.**
> All items shipped off-by-default (goldens bit-identical) with the standing bench/click gates.

Findings from a direct review of the shipped DSP (repo @ master) combined with the
virtual-analog literature. Ordered by musical-value-per-effort. Every item preserves goldens via
off-by-default parameters, carries the standing bench/click gates, and targets the ThinkPad budget.

## What the code review found

The DSP layer is clean — and that is precisely its limitation.

- **Oscillators start at phase 0 on every note** (`reset()` zeroes phase; voice note-on calls it for
  fresh voices). Every keypress produces bit-identical waveform alignment — the sterile
  "digital-perfect" attack. No phase randomization and no drift anywhere in the engine.
- **The SVF is exactly linear**, resonance hard-clamped at 0.98. No nonlinearity inside the filter
  loop; self-oscillation is structurally impossible; the pending "filter drive" R3 item is unbuilt.
  The TPT topology is the right foundation and the most under-exploited asset in the engine.
- **The reverb is textbook Freeverb** (8 parallel damped combs + 4 series allpasses per channel,
  static tunings). Correct, but static comb tunings read slightly metallic next to modern plates.
- **Voices are mono** until the pending unison work; the soft-clip output stage is the transparent
  spliced design (good — leave it).

The goal is **character, not perfection**: oscillators that drift slightly over time, filters with
nonlinearities, components whose interaction means the sound is never exactly the same twice.

## Tier 1 — Analog life for the oscillators (small effort, immediate)

**1a. Start-phase policy** (per-oscillator patch parameter): **RESET | RANDOM | FREE**.
- RESET = today (keep for punchy, consistent bass attacks — selectable, not removed).
- RANDOM = each note randomizes each oscillator's start phase — detuned stacks stop "flamming"
  identically and chords bloom differently every strike.
- FREE = oscillators run continuously; a note-on picks up wherever the phase is (true analog).
- **Default for new factory character patches: RANDOM.** (Param default stays RESET so goldens/Init
  are bit-identical; character presets set RANDOM in the final preset touch-up.) Cost: nil.

**1b. Analog drift**: a per-voice, per-oscillator slow random walk on pitch (bounded ±~2 cents,
sub-Hz update, control-rate) plus a hair of PW drift, scaled by one **global-per-part `analog`
amount** (0 = bit-exact today, default 0). Subtle = the "alive" quality of vintage polys; higher =
deliberate vintage wobble. Cost: a few control-rate randoms per voice. Tests: statistical bounds,
determinism under seeded RNG, golden-safe at 0.

## Tier 2 — The nonlinear filter (the single biggest sonic upgrade)

Build the pending "filter drive" as an **in-loop nonlinearity**, not a pre-filter waveshaper. The
celebrated analog filters owe their sound to saturation inside the feedback path (Huovilainen:
nonlinearities inside the filter sections; the Korg 35 literally uses a diode soft-clipper bounding
the feedback rather than clamping resonance away). For our TPT SVF (Zavalishin's framework):

- Add **drive** (input gain into the loop) and place **tanh saturation on the integrator inputs**
  (standard nonlinear-TPT; one or two tanh evals/sample — tanh cost dominates, so use a **fast
  rational tanh approximation, tested against `std::tanh` for tolerance**).
- **Extend resonance to 1.0+**: with the saturator bounding feedback, the filter **self-oscillates
  gracefully** (a playable keytracked sine at cutoff) instead of clamping at 0.98.
- Add **resonance loudness compensation** (passband dips as res rises in the linear SVF; the drive
  stage restores it).
- **Aliasing discipline**: nonlinearity aliases at base rate. Run the nonlinear filter at **2×**
  (cheap half-band up/down, or fold into the existing oscillator oversampling domain before
  decimation — **evaluate both, bench both**). This finally gives HQ mode a real job. Extend the
  aliasing "soul" test over driven/self-oscillating settings.
- **Params default**: drive 0 / res ≤ 0.98 bit-identical — a **drive=0 fast path that is literally
  today's linear code** keeps goldens and the ThinkPad budget honest (pay for tanh only when driven).
- **Bench gate**: driven-filter worst case, ThinkPad-derated, with the drive=0 fast path proven
  zero-cost.

## Tier 3 — Unison, done with character (lands with the unison item, task #96)

Random start phases per stack voice (mandatory — identical phases collapse the stack), a proper
**non-uniform detune spread curve**, per-stack-voice **stereo pan spread**, and the Tier-1 drift
applied per stack voice — the supersaw that sounds expensive. Voice-budget/headroom notes apply.

## Tier 4 — FX bloom (medium effort, big perceived polish)

- **4a. Modulated reverb tail**: a very slow, very small LFO modulation on a subset of the comb (or
  allpass) delay times — the cure for Freeverb's static/metallic tail; the tail chorus makes pads
  swim. Interpolated delay reads (the chorus already has the pattern), depth of a few samples, one
  new "motion" param defaulting 0 (golden-safe). Cheap and dramatic on pads.
- **4b. (Optional, later)** a second reverb algorithm — a Dattorro-style plate — only if 4a leaves
  appetite.
- **4c. Chorus thickening**: a second modulated tap per channel (dimension-style dual-tap) behind a
  `voices: 1|2` param, default 1.

## Packaging

One "Musicality Pass" package: Tier 1 (a+b) → Tier 2 (the centerpiece, its own gated increments:
linear-fast-path refactor, nonlinearity, self-osc + compensation, oversampling + bench) → Tier 4a →
4c, with Tier 3 folded into the unison item. Factory-preset touch-up at the end: RANDOM phase + a
breath of drift on character patches (strings/pads/brass), new presets showcasing driven/self-osc
filter (acid, industrial), reverb motion on the big pads.

Every increment: goldens bit-identical at defaults, click-torture in the same commit,
ThinkPad-derated bench with the drive=0/analog=0 fast paths proven free, ears-first sign-off on the
character defaults.

## Agreed contingencies (session note)

- **Unison ships default-off (1 voice)** with per-patch counts. If the ThinkPad report (#100) shows
  the budget can't take high counts, the Efficient/live profile **caps** unison to 2–3 while
  studio/HQ keeps 7, documented — **unison is never cut, only capped.**
- The **~109% derated worst case is unverified** (assumed ×3.5 derate); treat like every derated
  number — #100 settles it with real hardware data, and the voice-cap decision reopens with that data
  if the measured worst case genuinely exceeds budget (standing rule).
