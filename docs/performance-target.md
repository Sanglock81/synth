# Performance target record

The 1.0 CPU/timing record for the **minimum-spec reference target** — a 2015-class dual-core
laptop (Linux). The validation package lives in [`tools/target-validate/`](../tools/target-validate/);
run it on the target, paste the report back, and this doc gets the measured numbers appended and
the decisions settled. It is also the baseline for the post-1.0 performance review.

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
| **24-voice pool cap** | headroom for multitimbral (seq kit + looper patch + lead + spare); 24v+ALL-FX is the pool worst case | _pending_ — CONFIRMED / REOPENED |
| **Per-part FX** | 4 independent FX chains; 4-part ×4v + 4× ALL FX is the hard gate (still provisional) | _pending_ — CONFIRMED / REOPENED |
| **Always-oversample-when-driven** | drive/self-osc put the filter on the 2x path; clean patches pay nothing | _pending_ — CONFIRMED / REOPENED |
| **Unison live cap (Efficient ×3, HQ ×7)** | Efficient caps unison to 3 for CPU; the 7-member stack needs HQ | _pending_ — CONFIRMED / REOPENED |
| **FM cost (both depths, poly)** | phase-mod is a few extra ops/sample on sin/tri/WT carriers only | _pending_ — CONFIRMED / REOPENED |

## The ship decision rule (pre-agreed)

- **Zero** compute-overruns/xruns in the **realistic live-set bench row** *and* **both soaks**
  (128 and 256) ⇒ **ship as configured**, regardless of the synthetic worst-case percentages
  (unison HQ ×7, 24-voice driven+self-osc are headroom probes, not gates).
- **Overruns in realistic play** ⇒ present the pre-agreed mitigation options **for the user's
  call** (lower the default active-part count · a shared-reverb mode · a lower voice cap ·
  Efficient-only unison) — the decision is the user's, never absorbed here.

## Measured run (appended when the report comes back)

_Not yet run. Paste the report and this section fills in: machine/governor context, measured p99
per scenario, the derate factor(s), the settled decision table, and the soak overrun counts at
128 and 256._
