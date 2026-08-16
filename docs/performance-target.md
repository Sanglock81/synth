# Performance target record

> **CORRECTION (Aug 2026): the "dev box" IS the reference target.** The bench has run on this
> machine (Intel **i7-8650U** ThinkPad) all along — there is no separate slower laptop, and no files
> ever needed copying. So the **assumed ×3.5 dev→target derate was spurious**: it inflated
> target-native numbers ~3.5×. The corrected derate is **×1.0** (`kTargetDerate = 1.0`). Every
> "Target~ @×3.5" figure below is therefore ~3.5× too high; the real figures are just the measured
> p99 at the **performance** governor. The authoritative 1.0 record is **`docs/target-report.txt`**
> (from `tools/target-validate/validate.sh` run on this machine); this file is the superseded "before".

The 1.0 CPU/timing record for the **reference target** — the Intel i7-8650U ThinkPad this project is
developed and benched on (Linux). The validation package lives in
[`tools/target-validate/`](../tools/target-validate/); it builds JUCE-free with g++ and runs in place.
The measured numbers and settled decisions are in *Measured run* below; this is also the baseline for
the post-1.0 performance review. (The sections above the measured run are the pre-measurement "before".)

Constraint (see the deploy notes): DSP under ~30 % of a 128-sample block budget on the target
leaves headroom for the GUI, other tracks and OS jitter. Bench numbers are only valid at the
**performance** governor (`powersave` reads ~2x slow); `validate.sh` sets and restores it.

## Dev-box baseline (the "before", to compute the derate against)

Measured on the dev box (governor performance), 128-sample block, budget 2.667 ms.
`Target~` = dev p99 x the **assumed** derate of ×3.5 — the assumption this record replaces.
p99 is the robust worst case (max is OS-jitter dominated).

| scenario | dev p99 (ms) | Target~ p99 @×3.5 | % budget |
|---|---|---|---|
| Efficient 16-voice clean worst | 0.74 | 2.58 | 97 % |
| 16-voice + ALL FX | 1.01 | 3.52 | 132 % |
| 24-voice + ALL FX (pool cap) | 1.42 | 4.97 | 186 % |
| 16-voice driven filter (drive 1) | 1.05 | 3.68 | 138 % |
| **driven + self-oscillating filter, 24v** | 1.47 | 5.14 | 193 % |
| single kit + ALL FX (16v) | 0.72 | 2.50 | 94 % |
| 4-part ×4v + 4× ALL FX | 1.09 | 3.80 | 143 % |
| 2-part ×8v + 1× FX (realistic) | 0.94 | 3.30 | 124 % |
| **unison Eff ×3 (live cap), 12 notes** | 0.77 | 2.68 | 101 % |
| **unison HQ ×7 (contingency), 8 notes** | 2.02 | 7.07 | 265 % |
| **FM under polyphony, 24 notes** | 0.66 | 2.33 | 87 % |
| **Realistic live set (2-3 parts + kit + FX + LFOs)** | **1.10** | **3.87** | **145 %** |

**Read this honestly:** at the *assumed* ×3.5 the realistic live set projects to **145 %** of
budget — i.e. the assumption predicts overruns in real play. That makes the measured target run
**decisive**, not a rubber stamp: it tells us whether ×3.5 is pessimistic (the dev box is
newer/heavier-loaded than the original calibration) or whether we need a mitigation. Numbers in
**bold** are the newer scenarios (unison, FM, driven+self-osc, live set) and have **not yet been
measured on the target**.

## The report-processing contract

When the target report comes back, on the dev box we:

1. **Recompute the derate.** derate = dev p99 ÷ target p99, per scenario and a headline
   single-thread figure. **Replace the assumed ×3.5** in the bench harness, the bench header
   comment, this doc, and any comment citing ×3.5.
2. **Restate every provisional CPU decision** against measured numbers in the table below,
   each marked **CONFIRMED** or **REOPENED**.
3. **Apply the pre-agreed decision rule** (below). Never absorb the decision.
4. **Append the pasted report** to this doc (the 1.0 performance record + post-1.0 baseline).

## Provisional CPU decisions — to be settled by the measured run

| decision | current (provisional) basis | measured verdict |
|---|---|---|
| **24-voice pool cap** | headroom for multitimbral (seq kit + looper patch + lead + spare); 24v+ALL-FX is the pool worst case | **CONFIRMED** — see *Measured run* below |
| **Per-part FX** | 4 independent FX chains; 4-part ×4v + 4× ALL FX is the hard gate (still provisional) | **CONFIRMED** — see *Measured run* below |
| **Always-oversample-when-driven** | drive/self-osc put the filter on the 2x path; clean patches pay nothing | **CONFIRMED** — see *Measured run* below |
| **Unison live cap (Efficient ×3, HQ ×7)** | Efficient caps unison to 3 for CPU; the 7-member stack needs HQ | **CONFIRMED** — see *Measured run* below |
| **FM cost (both depths, poly)** | phase-mod is a few extra ops/sample on sin/tri/WT carriers only | **CONFIRMED** — see *Measured run* below |

## The ship decision rule (pre-agreed)

- **Zero** compute-overruns/xruns in the **realistic live-set bench row** *and* **both soaks**
  (128 and 256) ⇒ **ship as configured**, regardless of the synthetic worst-case percentages
  (unison HQ ×7, 24-voice driven+self-osc are headroom probes, not gates).
- **Overruns in realistic play** ⇒ present the pre-agreed mitigation options **for the user's
  call** (lower the default active-part count · a shared-reverb mode · a lower voice cap ·
  Efficient-only unison) — the decision is the user's, never absorbed here.

## Measured run — 2026-08-16 (the authoritative 1.0 record, `performance` governor)

Full run of `tools/target-validate/validate.sh` **on the reference target itself** (commit `0d1d1a7`),
at the **`performance`** governor. Raw report: [`target-report.txt`](target-report.txt).

- **Machine:** Intel **i7-8650U** (4c/8t) — *this* machine, the one the bench has always run on.
- **Governor:** `performance` — the valid measurement condition ([[vasynth-bench-governor]]). A prior
  `powersave` pass (conservative, ~2× slower) was also run as a cross-check; see the note below.
- **Measured derate: ×1.0.** The bench runs ON the target, so measured p99 *is* the target figure.
  The old assumed **×3.5 is retired** (bench `kTargetDerate`, this doc, the bundle README, CHANGELOG).

### Headline: old ×3.5 projection vs measured (×1.0, `performance`)

| scenario | old "Target~ @×3.5" | measured p99 (%budget) | verdict |
|---|---|---|---|
| Realistic live set (4 parts + FX + LFOs) | 145 % | **0.565 ms · 21 %** | OK<30 % |
| Per-part block-mod matrix (full 8-slot ×4) | — | 0.534 ms · 20 % | OK<30 % |
| 4-part ×4v + 4× ALL FX | 143 % | 0.531 ms · 20 % | OK<30 % |
| 24-voice + ALL FX | 186 % | 0.713 ms · 27 % | OK<30 % |
| 24-voice driven + self-osc | 193 % | 0.839 ms · 32 % | runs |
| Unison HQ ×7 (contingency probe) | 265 % | 1.132 ms · 42 % | runs |

Every scenario the ×3.5 assumption projected *over budget* lands comfortably **under** it; every live
configuration is `OK<30 %`. The only `OVERRUN` anywhere in the bench is the synthetic **HQ 4×
oversampling at 16 voices** probe — not a live config and not a gate.

### Provisional CPU decisions — settled against measured numbers (all CONFIRMED)

| decision | measured (p99, % budget) | verdict |
|---|---|---|
| **24-voice pool cap** | 24v+ALL FX 27 %; 24v driven+self-osc 32 % | **CONFIRMED** |
| **Per-part FX** | 4-part ×4v + 4× ALL FX = 20 % | **CONFIRMED** |
| **Always-oversample-when-driven** | 24v driven+self-osc 32 % | **CONFIRMED** |
| **Unison live cap (Eff ×3, HQ ×7)** | HQ ×7 42 % (contingency, not a gate) | **CONFIRMED** |
| **FM cost (both depths, poly)** | 24-note FM well under 30 % | **CONFIRMED** |
| **Per-part block-mod matrix cost** (6th row) | full 8-slot ×4 parts = 20 % | **CONFIRMED** |

### Soak (compute-overrun proxy, ALL FX, 600 s each) — both PASS, zero overruns

| block | budget | blocks | overruns | mean / p99 / max | verdict |
|---|---|---|---|---|---|
| **128** | 2.667 ms | 1,862,260 | **0** | 0.32 / 0.39 / 0.96 ms | **PASS** |
| **256** | 5.333 ms | 956,543 | **0** | 0.63 / 0.82 / 1.79 ms | **PASS** |

### Ship-rule status — the bright line is MET; the ship call is the user's

The pre-agreed rule requires **zero** xruns in the realistic set **and both** soaks. At `performance`:
realistic-set bench 21 % `OK<30 %`, **@128 soak 0 overruns, @256 soak 0 overruns** — over ~2.8M blocks
(2.8 hours of flat-out audio). **Clause A of the ship rule is satisfied.** Per the standing agreement the
tag itself still fires only on the user's explicit call (after UAT + signature); this record establishes
that performance is not a blocker.

**Cross-check — the earlier `powersave` run (superseded, kept for the record).** Before the governor was
set, a `powersave` pass logged **11 @128 overruns (0.0010 %)** in a jitter burst (max 5.36 ms vs 0.54 ms
mean; @256 clean). The `performance` re-run proves that was governor jitter, not DSP cost: the same @128
soak's **max block fell from 5.36 ms to 0.96 ms** (a third of budget) with **zero** overruns. This is the
empirical basis for documenting the `performance` governor as a low-latency-live requirement (INSTALL/README).
