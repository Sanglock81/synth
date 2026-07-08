# ThinkPad validation package

Self-contained CPU/timing validation for the **live target** — the 2015 ThinkPad
X1 Carbon (3rd gen, dual-core Broadwell, Linux). Copy this whole folder to the
ThinkPad and run `./validate.sh`. It produces **`thinkpad-report.txt`** — paste that
back. Its measured numbers become the **real derate factor**, replacing the assumed
`×3.5` used in the dev-box bench gates.

## Why build-from-source (not a prebuilt bundle)

The DSP layer (`Source/DSP/`) is **100% JUCE-free**, and the benchmark is literally
`g++ bench_engine.cpp -ISource/DSP` — no JUCE, no cmake, no network fetch. So the most
reliable path is to **compile on the ThinkPad itself**:

- The only dependency is a C++17 compiler (`g++`); if it's missing the script tells you
  `sudo apt-get install -y build-essential`. That's the entire "dev setup".
- Compiling **on the target** with `-march=native` gives the ThinkPad's *own* optimal
  codegen — the honest measurement, not a cross-built binary that might use instructions
  Broadwell lacks (a prebuilt from the Ryzen dev box risks SIGILL / different codegen).
- No glibc/ABI mismatch, no shared-library surprises.

The bundled `dsp/` and `bench/` are a **snapshot** of the repo's JUCE-free sources,
refreshed by `assemble.sh` on the dev box (re-run it after Sub-phase 2 so the 4-part
configs are included, then re-copy this folder).

## What `validate.sh` does

1. **Governor** — saves the current CPU governor, sets `performance` (via `sudo`,
   prompting once), and **restores it on exit**. If it can't, it says so and flags the
   numbers as invalid (a `powersave` run reads ~2× slow and must not be used as a gate).
2. **Context** — prints governor, per-core current/min/max frequency, turbo state,
   `lscpu`, `cpupower frequency-info`, thermal zones, kernel — so every number is tied
   to a known clock state.
3. **Bench** — builds and runs `dsp_bench` (the same benchmark as the dev gates:
   Efficient 16-voice worst case, all-FX, the kit worst case; **plus the 4-part
   per-part-FX and 2-part realistic cases once Sub-phase 2 lands and `assemble.sh` is
   re-run**).
4. **Soak** — a 10-minute `soak` run (`SOAK_SECS=` to change): the real DSP path
   (engine + all FX, pool saturated, continuous note-on/off/steal storm) flat-out for
   the duration. It counts **compute overruns** (blocks whose render exceeds the
   real-time budget = what would xrun) and stresses the CPU for thermal throttling.
   This is a portable proxy that needs no audio device or MIDI port; the real
   device-buffer picture is the PipeWire section below.
5. **PipeWire** — reports `clock.rate`/`clock.quantum`, and forces the quantum to
   **128** and **256** to record the rate/latency the derate applies at (restores auto).

## Interpreting the report

- The **bench** `p99 × (measured derate)` vs the 2.667 ms budget is the gate. The
  measured single-thread ratio dev-box-p99 ÷ thinkpad-p99 is the **real derate** — send
  it back and we replace the assumed `×3.5` in `tests/bench/bench_engine.cpp` and the
  bench docs (recording measured-vs-assumed).
- The **soak** should show `COMPUTE OVERRUNS: 0`. Any overruns, or a max block time
  climbing over the run (thermal throttling), is the signal to look at the pre-agreed
  options (part-count default, shared-reverb mode, documented guidance).

## Files

| file | purpose |
|------|---------|
| `validate.sh` | entry point (run this on the ThinkPad) |
| `soak_harness.cpp` | JUCE-free 10-min compute-overrun soak |
| `bench/bench_engine.cpp` | snapshot of the dev bench |
| `dsp/` | snapshot of the JUCE-free DSP headers |
| `assemble.sh` | (dev box) refresh the snapshot from the repo |
| `BUNDLE_INFO` | git commit the snapshot came from |
