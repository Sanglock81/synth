# VA Synth

A hand-rolled virtual analog polysynth. C++17 / JUCE 8. Builds as a **VST3
plugin and a standalone app from the same code**, on Linux and Windows.

Designed for: Korg B2 (primary keyboard) + Novation Launchkey Mini
(secondary keys + mappable CC controller) → Focusrite Scarlett 2i2 out.

## Architecture

```
                    ┌────────────────────────────────────────────┐
 MIDI in ──────────▶│ processBlock                               │
 (Korg B2 +         │   ├─ note on/off ──▶ SynthEngine           │
  Launchkey, or     │   ├─ CC ───────────▶ MidiLearnManager ──┐  │
  DAW routing)      │   │                                     ▼  │
                    │   │                            APVTS params │
 GUI / DAW ────────▶│ APVTS ◀─────────────────────────────────┘  │
 automation         │   │ snapshot once per block                │
                    │   ▼                                        │
                    │ SynthEngine (16 voices, global LFO)        │
                    │   voice: OSC1+OSC2+noise → SVF → VCA       │
                    │          (PolyBLEP)     (TPT)  (2× ADSR)   │
                    │   ▼                                        │
                    │ mono → stereo → master gain → out          │
                    └────────────────────────────────────────────┘
```

Key rules baked into the design:

* **Audio thread is sacred.** `SynthEngine` and everything under `Source/DSP/`
  is allocation-free and lock-free. Parameters cross the thread boundary via
  APVTS atomics, snapshotted once per block into a POD `VoiceParams`.
* **Voices are dumb.** They hold DSP state only, never parameter state.
  All APVTS access lives in `PluginProcessor::snapshotParams()`.
* **MIDI is sample-accurate.** `processBlock` renders up to each event's
  sample position before dispatching it.
* **Parameter IDs are forever.** Once presets exist, never rename an ID in
  `Parameters.h` — add new ones instead.

## File map

| File | What it is |
|---|---|
| `Source/Parameters.h` | Every parameter ID + APVTS layout. Single source of truth. |
| `Source/DSP/PolyBlepOscillator.h` | Anti-aliased saw/square(PWM)/tri/sine. The character. |
| `Source/DSP/SVFilter.h` | TPT state-variable filter (Simper/Cytomic). LP/HP/BP/Notch. |
| `Source/DSP/ADSREnvelope.h` | Exponential-segment ADSR with click-free retrigger + steal fade. |
| `Source/DSP/LFO.h` | Global control-rate LFO (tri/sine/square/S&H). |
| `Source/DSP/SynthVoice.h` | One voice's full signal chain. |
| `Source/DSP/SynthEngine.h` | Voice pool, oldest-note stealing, LFO routing. |
| `Source/MidiLearnManager.h` | (channel, CC) → parameter mapping; Launchkey default map. |
| `Source/PluginProcessor.*` | JUCE seam: MIDI dispatch, param snapshot, render, state. |
| `Source/PluginEditor.*` | Thin wrapper over GenericAudioProcessorEditor (v1 GUI). |

## Build

### Linux (Ubuntu/Debian)

```bash
sudo apt install build-essential cmake git \
    libasound2-dev libjack-jackd2-dev libfreetype-dev libfontconfig1-dev \
    libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxext-dev \
    libcurl4-openssl-dev libwebkit2gtk-4.1-dev

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Outputs:
* Standalone: `build/VASynth_artefacts/Release/Standalone/VA Synth`
* VST3: `build/VASynth_artefacts/Release/VST3/` (auto-copied to `~/.vst3`)

### Windows

Visual Studio 2022 (Desktop C++ workload) + CMake. Same two commands from a
Developer Prompt. The standalone uses WASAPI/DirectSound by default; for
lowest latency into the Scarlett, load the VST3 in Ableton (ASIO) or add
ASIO SDK support later.

### First run (standalone, Linux)

1. Launch the standalone. Open **Options → Audio/MIDI Settings**.
2. Output device: Scarlett 2i2 (via ALSA, JACK, or PipeWire's JACK layer).
   Buffer 128–256 @ 48 kHz to start.
3. Enable **both** MIDI inputs: Korg B2 and Launchkey Mini.
4. Play the Korg; twist Launchkey knobs (CC 21–28 pre-mapped: cutoff, reso,
   osc mix, env amt, attack, release, LFO rate, LFO depth).

### Laptop-only operation (no hardware)

**Audio device fallback.** The standalone's Audio/MIDI Settings offers device-type
selection (ALSA + JACK are compiled in) and any output including built-in laptop
audio. If a previously-selected device is gone at launch (Scarlett unplugged), it
falls back to the system default automatically — it won't crash or launch silent
(`selectDefaultDeviceOnFailure`). The dialog shows which device is active.

**QWERTY computer keyboard.** The standalone editor plays notes from the computer
keyboard — chromatic, one semitone per key, left to right (US layout):

```
  q w e r t y u i o p [ ]     C4 … B4   (MIDI 60–71)
  1 2 3 4 5 6 7 8 9 0 - =     C5 … B5   (MIDI 72–83)

  z / x  = octave shift down / up   (extends to the full range for bass patches)
```

Fixed velocity 0.8. OS auto-repeat is ignored (clean one note-on / one note-off
per press). Notes merge into the same engine path as hardware MIDI and coexist
with it. Keys are ignored while a text field is focused; all notes release when
the window loses focus (Alt-Tab) or closes — no stuck notes. Standalone only; in
a plugin the host owns the keyboard.

## Roadmap

**v1 (make it sound good)**
- [x] Verify PolyBLEP output — automated aliasing test (FFT, ≤−60 dB, naive fails).
      Oscillator is now 4× oversampled with a configurable quality/CPU tradeoff
      (`Efficient` audible-band-clean default, `HQ` full-band-clean for studio).
- [ ] Pitch bend + mod wheel + sustain pedal handling (Korg B2 sends all)
- [ ] Mono/legato modes + glide (slew note frequency in `SynthVoice`)
- [ ] Smooth per-block parameter changes (one-pole smoothing on cutoff/gain
      to kill zipper noise)
- [x] Persist MIDI-learn mappings in APVTS state

**Deployment / validation (not yet done)**
- [ ] ThinkPad X1 Carbon 3rd gen (2015, dual-core Broadwell) is the live Linux
      machine: measure real round-trip MIDI-to-audio latency of the standalone
      under PipeWire at 128 samples / 48 kHz, and document recommended buffer
      settings (128 vs 256) for that machine. DSP headroom is benched
      (`tests/bench/dsp_bench`); this is the end-to-end I/O latency check.
      **This is also the real worst-case gate for the voice cap** (default 12):
      dev-box p99 is scheduler-jitter-contaminated, so confirm no xruns on the
      ThinkPad and adjust `VASYNTH_MAX_VOICES` if needed.

**Considered and deferred**
- Internal multi-core audio (splitting voices across RT worker threads): NOT
  done. Single-core Efficient at 12 voices is ~23% of the ThinkPad budget
  (median), and on a 2-core machine internal threading competes with PipeWire on
  core 1 and adds RT jitter. The DAW already parallelizes across instances.
  Revisit only if HQ-live or large unison is needed (v2, needs a lock-free pool).

**v2 (make it deep)**
- [ ] Mod matrix (any source → any destination, replaces single LFO dest)
- [ ] Second LFO, per-voice LFO option, MIDI-clock sync
- [ ] Unison/detune stacking, hard sync osc2→osc1
- [ ] Sub-oscillator, filter drive/saturation
- [ ] Custom GUI with right-click MIDI-learn
- [ ] Preset browser

**Reference reading (open source to study, not copy blindly)**
- amsynth — closest architectural sibling, small and readable (GPL)
- Surge XT — filter algorithms (`sst-filters` is a reusable library), MSEG
- JUCE examples — `AudioPluginDemo` for processor patterns
