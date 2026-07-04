# VA Synth test suite

Catch2 v3 (via CMake FetchContent), wired into CTest. Two binaries:

| Target | Links | Covers |
|---|---|---|
| `dsp_tests` | **no JUCE** — only `Source/DSP/` | oscillator, filter, envelope, LFO, engine, RT-safety (zero-alloc), golden render |
| `plugin_tests` | JUCE + the processor | APVTS state round-trip, MIDI-learn behaviour + persistence |

The DSP tests are deliberately JUCE-free: `Source/DSP/` is standard-library-only
by design, and a test that needed JUCE to exercise an oscillator would be a
design regression.

## Running

```bash
# Everything (build both artefacts + all tests + pluginval), non-zero on failure:
./run-all-checks.sh

# Or, after a `-DVASYNTH_BUILD_TESTS=ON` build:
cd build && ctest --output-on-failure

# A single binary directly:
./build/tests/dsp_tests            # all DSP cases
./build/tests/dsp_tests "[osc]"    # by tag
./build/tests/plugin_tests
```

## Notable tests

* **Aliasing (`[soul]`)** — renders a saw at a *non-degenerate* ~3 kHz (exactly
  3 kHz at 48 kHz folds aliases onto harmonics and tests nothing), FFTs it with a
  Blackman-Harris window, and asserts every non-harmonic peak is below −60 dB.
  A naive saw must fail it; passing requires the 4× oversampled oscillator.
* **RT-safety** — 1000 render blocks under a global-`new` counting guard;
  `SynthEngine::render` must allocate zero.
* **Golden render** — a fixed 2 s chord + filter sweep compared to a committed
  reference WAV (`golden/render.f32.wav`) within a small tolerance. Regenerate
  by deleting the file and re-running.

## pluginval

`run-all-checks.sh` fetches the Linux prebuilt into `tools/` (git-ignored) and
CTest runs it against the built VST3 at strictness level 8.
