# Osc FM (phase-modulation) chain — #132

The scoped R3 "oscillator cross-modulation" item, landed before the ThinkPad gate (#100). Ring
modulation and hard sync are the remaining cross-mod features and move to **1.1**; this delivered the
FM half. After this, the 1.0 feature freeze closes permanently.

## What it is

DX-style **phase modulation** (universally marketed as "FM"). Per sample, a modulator oscillator's
output offsets the *read phase* of a carrier oscillator while the carrier's phase accumulator keeps
advancing unmodulated. A simple carrier (a sine) then grows a band of sidebands at `fc ± n·f_mod`
whose amplitudes follow Bessel functions of the **modulation index** `β`.

Chain direction (fixed): **osc3 → osc2 → osc1**.
- `osc1_fm` = depth of **osc2 modulating osc1**.
- `osc2_fm` = depth of **osc3 modulating osc2**.
- osc3 tops the chain (nothing modulates it), so it has no FM knob.

## Depth → modulation index

`phaseOffset(cycles) = depth · kFmDepthToCycles · modOut`, with `kFmDepthToCycles = 1.0` and the
modulator output in `[-1, 1]`. So at a full-scale sine modulator, `β = 2π·depth` radians:

| depth | β ≈ | character |
|------:|----:|-----------|
| 0.15  | 0.9 | subtle warmth |
| 0.30  | 1.9 | E-piano / mild bell |
| 0.50  | 3.1 | bright bell |
| 1.00  | 6.3 | aggressive / metallic |

The **modulator uses its raw output**, independent of its mix level — so a modulator at level 0 is
inaudible on its own yet still shapes its carrier (the classic 2-operator patch). The modulator's
**OCTAVE / SEMI** set the FM **ratio** (integer ratios → harmonic, non-integer → inharmonic/bell), and
because both oscillators derive frequency from the same note, the ratio **keytracks by construction**.

## Carrier restriction (SIN / TRI / WT only)

FM applies only when the *carrier* wave is sine, triangle, or wavetable — continuous-phase waves that
can be read at an offset. Saw and square rely on PolyBLEP edge corrections computed at the *un-offset*
phase; a phase offset invalidates them. On a saw/square carrier the depth is forced to 0 in the DSP
**and** the UI knob is disabled + dimmed with a tooltip. Modulators are unrestricted (any wave).

## Click-safety & goldens

The live part's FM depths are one-pole smoothed in the engine (like osc levels / filter drive), so
knob drags, automation, and LFO→FM sweeps are click-free (proven by the depth-sweep click-torture).
Depth defaults to 0, and a 0 depth takes the exact un-modulated code path (`phaseOffset == 0` reads
`phase` directly) — so every existing patch and the committed goldens are **bit-identical**.

## Honest limitation — aliasing at extremes

The PM is computed at the base rate (the modulator value is held across the carrier's oversample
sub-steps), so sidebands that exceed Nyquist **fold** — inherent to through-zero-free digital FM (the
DX7 had it too). A sine carrier under PM is intrinsically bounded to `|·| ≤ 1` (no amplitude blowup),
and the WT/oversampled carrier still band-limits its *own* harmonics, but very high notes at maximum
depth will alias. This is bounded, musical "edge" (see the **Sideband Growl** patch) rather than a
defect; if a cleaner high register is ever wanted, oversampling the whole PM computation is the fix
(deferred — it costs CPU the ThinkPad target doesn't have to spare).

## Mod matrix

`osc1_fm` / `osc2_fm` are registry destinations **"Osc 1 FM" / "Osc 2 FM"** (voice-tier, like the
osc levels — applied per-voice via `evaluate()`, not double-applied at block rate). The headline route
is **Velocity → FM depth**: a harder hit raises the modulation index for the classic DX
brighter-when-struck-harder response. RANDOM includes FM depths, weighted mostly-0 with an occasional
tasteful amount and a rare wild one (`randFmNorm`).

## Content

Three showcase patches (bank 123 → 126): **FM E-Piano** (Keys, ratio 1:1, velocity→FM), **Bell Ratio**
(Pluck, inharmonic ~3.36:1), **Sideband Growl** (Experimental, 3-operator stack into a driven filter,
LFO→FM movement). Velocity brightness on the tonal FM patches comes from the velocity→FM route, not
velocity→cutoff (DX-authentic).

## Tests

- `tests/dsp/test_fm.cpp` — sideband proof (symmetric `fc ± n·f_mod`), depth-0 bit-identity, depth
  broadens the spectrum, inharmonic ratio, keytrack, bounded at high-note+max-depth, engine chain runs
  a level-0 modulator, saw-carrier inertness.
- `tests/plugin/test_fm_plugin.cpp` — Velocity→Osc1Fm brightens harder notes; mid-note depth-sweep is
  click-free; processBlock stays allocation-free with FM live.
- `tests/plugin/test_ui_smoke.cpp` — the FM knob exists on osc1/osc2 and enables only for a
  sine/tri/WT carrier (`osc-fm.png`).
