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

## Measured run — 2026-08-15 (the authoritative 1.0 record)

Full run of `tools/target-validate/validate.sh` **on the reference target itself** (commit `717462c`).
Raw report: [`target-report.txt`](target-report.txt).

- **Machine:** Intel **i7-8650U** (4c/8t) — *this* machine, the one the bench has always run on.
- **Governor:** `powersave` (not `performance`). **This makes every number below CONSERVATIVE**: the
  CPU ran ~2× slower than at `performance`, so the true target figures are roughly **half** of what
  is shown. Powersave numbers can't produce a false *pass* — a pass here is a pass with margin.
- **Measured derate: ×1.0.** The bench runs ON the target, so measured p99 *is* the target figure.
  The old assumed **×3.5 is retired** (bench `kTargetDerate`, this doc, the bundle README, CHANGELOG).

### Headline: old ×3.5 projection vs measured (×1.0, powersave — halve again for `performance`)

| scenario | old "Target~ @×3.5" | measured p99 (powersave, %budget) | verdict |
|---|---|---|---|
| Realistic live set (4 parts + FX + LFOs) | 145 % | **1.525 ms · 57 %** | runs |
| Per-part block-mod matrix (full 8-slot ×4) | — | 1.242 ms · 47 % | runs |
| 24-voice + ALL FX | 186 % | 1.160 ms · 44 % | runs |
| 24-voice driven + self-osc | 193 % | 1.525 ms · 57 % | runs |
| Unison HQ ×7 (contingency probe) | 265 % | 2.289 ms · 86 % | runs |
| FM, 24 notes | 87 % | 0.627 ms · 24 % | OK<30 % |

Every scenario that the ×3.5 assumption projected *over budget* is comfortably **under** it. The only
`OVERRUN` in the whole bench is **HQ 4× oversampling at 16 voices (112 %)** — a synthetic headroom
probe (nobody plays 16 voices of 4× HQ oversampling), not a live configuration or a gate.

### Provisional CPU decisions — settled against measured numbers

| decision | measured (powersave p99, % budget) | verdict |
|---|---|---|
| **24-voice pool cap** | 24v+ALL FX 44 %; 24v driven 58 %; 24v driven+self-osc 57 % — all *runs* | **CONFIRMED** |
| **Per-part FX** | 4-part ×4v + 4× ALL FX = 33 % | **CONFIRMED** |
| **Always-oversample-when-driven** | 16v driven 31 %, 24v driven 58 % | **CONFIRMED** |
| **Unison live cap (Eff ×3, HQ ×7)** | Eff ×3 38 %; HQ ×7 86 % (contingency, not a gate) | **CONFIRMED** |
| **FM cost (both depths, poly)** | 16-note 19 %, 24-note 24 % | **CONFIRMED** |
| **Per-part block-mod matrix cost** (6th row) | full 8-slot ×4 parts = 47 % | **CONFIRMED** |

### Soak (compute-overrun proxy, ALL FX, 600 s each)

| block | budget | blocks | overruns | mean / p99 / max | verdict |
|---|---|---|---|---|---|
| **256** | 5.333 ms | 585,981 | **0** | 0.54 / 0.89 / 3.53 ms | **PASS** |
| **128** | 2.667 ms | 1,116,056 | **11** (0.0010 %) | 0.54 / 0.89 / **5.36 ms** | overruns present |

### Ship-rule status — the user's call (not absorbed here)

The pre-agreed rule requires **zero** xruns in the realistic set **and both** soaks. The realistic-set
bench and the @256 soak are clean, but the **@128 soak logged 11 overruns**, so the bright line is
**not** met and this is **not** an automatic ship. The evidence *characterising* those 11 (for the
decision, not to wave them away):
- They arrived in a **burst** — 0 through the first 120 s, then +10 in one ~30 s window, +1 later, then
  **none for the final ~6 minutes**. That is a transient scheduling/frequency event, not sustained load.
- **max 5.36 ms while mean is 0.54 ms and p99 is 0.89 ms** (33 % of budget) — the DSP is nowhere near
  budget; a lone 10× spike is OS jitter.
- The **@256 soak on the same DSP was perfectly clean** (max 3.53 ms = 66 % of its budget).
- All of this is at **powersave**, which both inflates cost ~2× and is more jitter-prone.

The single clean way to convert this to a valid zero is a **`performance`-governor re-run** (needs sudo,
declined this pass). Absent that, the pre-agreed mitigation menu is the user's to pick from: lower the
default active-part count · a shared-reverb mode · a lower voice cap · Efficient-only unison. **Decision
pending the user; the tag does not fire until they make it.**
