> **CORRECTION (Aug 2026): this bundle runs ON the reference target (Intel i7-8650U).**
> The old assumed **x3.5** dev->target derate was spurious (there is no separate machine); the
> corrected derate is **x1.0** — measured p99 at the performance governor IS the target figure.

# Minimum-spec target validation package

Self-contained CPU/timing validation for the **live target** — a 2015-class dual-core
machine (Linux). Copy this whole folder to the
target and run `./validate.sh`. It produces **`target-report.txt`** — paste that
back. Its measured numbers become the **real derate factor**, replacing the assumed
`x3.5` used in the dev-box bench gates, and settle every provisional CPU decision.

## The one command

```sh
./validate.sh            # FULL ship gate: build + bench + 10-min soak @128 AND @256 + PipeWire
./validate.sh --quick    # build + bench only, NO soak (iteration; the report marks itself QUICK)
SOAK_SECS=60 ./validate.sh   # shorter soak for a smoke test
```

Expected duration: **~10–12 min** full (dominated by the two soaks: 10 min each unless
`SOAK_SECS` is lowered — wait, they run back to back, so a 600 s soak x2 = ~20 min; set
`SOAK_SECS=300` for ~10 min total if you want it shorter). `--quick` is well under a minute.

## Why build-from-source (not a prebuilt bundle)

The DSP layer (`Source/DSP/`) is **100% JUCE-free**, and the benchmark is literally
`g++ bench_engine.cpp -Idsp` — no JUCE, no cmake, no network fetch. So the most reliable
path is to **compile on the target itself**:

- The only dependency is a C++17 compiler (`g++`); if it's missing the script prints a
  `SKIPPED:` line telling you `sudo apt-get install -y build-essential`. That's the entire
  "dev setup" — there is no dev environment to install.
- Compiling **on the target** with `-march=native` gives the target's *own* optimal
  codegen — the honest measurement, not a cross-built binary that might use instructions
  the target lacks (a prebuilt from the dev box risks SIGILL / different codegen).
- No glibc/ABI mismatch, no shared-library surprises.

The bundled `dsp/` and `bench/` are a **snapshot** of the repo's JUCE-free sources,
refreshed by `assemble.sh` on the dev box (BUNDLE_INFO records the git commit). Re-run
`assemble.sh` after any DSP/bench change, then re-copy this folder.

## What `validate.sh` does

1. **Governor** — saves the current CPU governor, arms a restore trap **before** touching
   anything (so Ctrl-C mid-run still restores), sets `performance` (via `sudo`, prompting
   once), and restores the prior governor on **any** exit. If it can't set performance it
   flags every number as INVALID (a `powersave` run reads ~2x slow and must not be a gate).
2. **Context** — governor, per-core current/min/max frequency, turbo state, `lscpu`,
   `cpupower frequency-info`, thermal zones, kernel, and the built-from git hash (BUNDLE_INFO)
   — so every number is tied to a known clock state and commit.
3. **Bench** — builds + runs `dsp_bench`, covering: 16- and 24-voice clean worst cases;
   the 6A/6B osc + FX sweeps; the **2C driven** and **driven + self-oscillating** paths;
   the **Sub-phase 1 kit** and **Sub-phase 2 4-part per-part-FX** worst cases; **#96 unison**
   at the Efficient live cap (x3) and the 7-member contingency (HQ); **#132 FM** under
   polyphony (both depths); and the **realistic live set** (lead + unison pad + FM/driven
   bass + sequenced kit, all FX + LFOs).
4. **Soak** — the `soak` binary run **at block 128 AND 256** (`SOAK_SECS=` to change):
   the real DSP path (engine + all FX, pool saturated, continuous note-on/off/steal storm)
   flat-out for the duration. It counts **compute overruns** (blocks whose render exceeds
   the buffer's real-time budget = what would xrun) and stresses the CPU for thermal
   throttling. Portable proxy — needs no audio device or MIDI port.
5. **PipeWire** — reports `clock.rate`/`clock.quantum`, forces the quantum to **128** and
   **256** to record the rate/latency the derate applies at (restores auto).

Anything that can't run degrades to a clearly-marked `SKIPPED: <reason>` line rather than
dying silently — a partial report never masquerades as a pass. `--quick` runs bench only and
stamps the report `MODE: QUICK` so it can't be mistaken for the gate.

## The report-processing contract (what happens when you paste it back)

When `target-report.txt` comes back to the dev box, we:

1. **Recompute the derate.** Compute the measured derate factor(s) = dev-box p99 ÷
   target p99 (per scenario, and a headline single-thread figure) and **replace the
   assumed `x3.5`** everywhere it appears — `tests/bench/bench_engine.cpp` (`kTargetDerate`),
   the bench header, `docs/target-1.0-perf.md`, and any code comment that cites `x3.5`.
2. **Restate every provisional CPU decision** against measured numbers, in one table —
   **24-voice cap · per-part FX · always-oversample-when-driven · unison live cap · FM cost**
   — each marked **CONFIRMED** or **REOPENED**.
3. **Apply the pre-agreed decision rule.** *Zero* compute-overruns/xruns in the **realistic
   live-set bench row** and **both soaks** ⇒ **ship as configured**, regardless of the
   synthetic worst-case percentages (those are headroom probes, not gates). Overruns in
   realistic play ⇒ we present the pre-agreed mitigation options (lower default part count /
   shared-reverb mode / voice-cap / Efficient-only unison) **for the user's call** — the
   decision is never absorbed silently.
4. **Commit the report** to `docs/target-1.0-perf.md` (or alongside it) — it is the 1.0
   performance record and the baseline for the post-1.0 performance review.

## Interpreting the report at a glance

- **Soak** `COMPUTE OVERRUNS: 0` at both 128 and 256, and a `max` block time that does **not**
  climb over the run (no thermal throttling) — this is the ship signal.
- **Bench** `realistic live set` row: its `target~ p99` (once the derate is remeasured) well
  under the 2.667 ms budget.
- The synthetic worst cases (unison HQ x7, 24-voice driven+self-osc) may show high `%budget` —
  that's expected and is **not** the gate; it tells us where the margin is.

## Files

| file | purpose |
|------|---------|
| `validate.sh` | entry point (run this on the target); `--quick` for bench-only |
| `soak_harness.cpp` | JUCE-free compute-overrun soak (block size is arg 2) |
| `bench/bench_engine.cpp` | snapshot of the dev bench |
| `dsp/` | snapshot of the JUCE-free DSP headers |
| `assemble.sh` | (dev box) refresh the snapshot from the repo |
| `BUNDLE_INFO` | git commit + UTC the snapshot came from |
