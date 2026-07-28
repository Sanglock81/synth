# RANDOM redesign — two buttons, one algorithm (CORRECTED decision)

Supersedes the earlier "keep archetype/constrained/wild inside wild" note. **The mode split is
removed.** Two buttons, two jobs, nothing else:

- **VARY — unchanged.** The ±walk works; same button, logic, tests, placement next to RANDOM. It is
  *why the modes can go*: "usable variation near a sound I like" is VARY's job, done better than any
  generation mode. (Stated deliberately: VARY is KEPT.)
- **RANDOM — one algorithm, every press.** Remove the archetype/constrained/wild probability roll,
  the mode toast, and the long-press mode picker. One press = one algorithm, always.

**Kept:** VARY; focused-part scope; the standing exclusion list (master, routing, poly mode, glide,
…); seeded determinism for tests; the 200× hammer test.

## The one algorithm — full random support, musical density

Every continuous parameter still spans its **full range** — nothing reachable by hand is unreachable
by RANDOM. Only the **probability shaping** changes, so useful sounds happen far more often:

1. **Perceptual distributions.** All **time** params (envelope stages, free-mode LFO rates, glide if
   ever included) and all **frequency** params (cutoff, EQ freqs) sample **log-uniformly**, not
   linearly (5 ms vs 50 ms attack is a bigger event than 20.0 vs 20.05 s). Levels/mixes sample with a
   **mild bias toward the useful middle** (beta-like); extremes still reachable.
2. **Broken-patch invariants — defect culling, never character culling.** Post-generation repair
   pass, each rule fixing ONLY the offending parameter (the rest of the roll stands):
   - at least one oscillator audible (existing floor);
   - filter not fully closed against the only sounding register (cutoff below the lowest sounding
     fundamental → re-roll cutoff alone);
   - attack and release not BOTH at sluggish extremes with near-zero sustain (re-roll one);
   - level staging normalized so the patch isn't near-silent by multiplication.
3. **Coherence latent.** Per press, roll one hidden **temperament** scalar (0 = percussive … 1 =
   sustained) and **loosely correlate** envelope stages, release, and reverb size toward it —
   correlation, not determinism; every parameter keeps independent variance. This is what makes a
   random patch feel intentional rather than assembled.
4. **SEMI randomizes musically.** The osc semi params join the scope, quantized to
   `{0, 0, 0, ±5, ±7, ±12}` (zero weighted heaviest) with a small chance of a chromatic outlier.
5. **Resonance.** Usual cap for most rolls, with a **~5%** high-reso excursion into the
   self-osc-adjacent zone — the occasional screamer is wanted; a constant diet isn't.
6. **Mod-matrix routes (kept from wild, simplified).** Every press rolls **0–3 routes** from valid
   registry pairs. This was the best part of wild; it stays in the one algorithm.
7. **Optional re-roll check (feasibility-gated).** After generation, render ~250 ms offline; if
   silent / DC-stuck / numerically broken, silently re-roll (≤5 attempts, then ship the last roll
   regardless). **If it costs >~150 ms wall-clock per press, skip it and say so** — the invariants
   already catch most defects. (Feasibility: the scratch-render path exists via `bakePresetParams`;
   250 ms ≈ 24 blocks ≈ well under 2 ms, so ≤5 tries ≈ ≤10 ms — expected cheap, but measure.)

## UI

RANDOM is a single button: **no toast, no long-press mode picker** (long-press returns to the
default/unassigned gesture rules). VARY stays beside it, unchanged.

## Tests

- VARY's tests untouched.
- **Remove** the mode-split tests.
- **Add** distribution sanity (seeded: attack times log-spread; SEMI values in the quantized set;
  temperament correlation present but loose).
- Each **invariant** gets a directed test (seed chosen to trigger the rule; assert the repair).
- **Hammer 200×** stays, now asserting non-silence AND the invariants across all presses.
- Determinism under a fixed seed.

## Docs / help

One line each: RANDOM ("fully random patch, shaped toward playable"), VARY ("small step from the
current sound"); state the NEW / NEIGHBORHOOD pairing so the two-button model is self-explanatory.
Remove mode-picker references everywhere (README, help overlay, tooltips).

## Honest expectations (for the wrap-up)

The **bias strengths** and the **temperament correlation looseness** are **ear-tuned constants** —
name them in ONE place. The user runs press-it-twenty-times sessions in UAT and reports keeper-rate;
expect one or two constant-tuning rounds. **Ship the first calibration conservatively (mild biases)**
so the "fully random" character stays honest.
