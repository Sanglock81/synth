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
                    │   voice: OSC1+OSC2+OSC3+noise → SVF → VCA  │
                    │   per-source mix + kill  (TPT)  (2× ADSR)  │
                    │   (PolyBLEP)  velocity → amp & cutoff       │
                    │   ▼                                        │
                    │ mono → stereo → FX chain → master → out    │
                    │   reorderable: chorus/delay/reverb/width   │
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
  `Parameters.h` — add new ones instead. When a control's *meaning* changes, the
  old ID stays registered and a migration runs on load. The 2→3-oscillator move
  (6A) froze the old `osc_mix` crossfade and derives the new independent
  `osc{1,2,3}_level` faders from it, so pre-6A sessions **and saved presets** load
  sounding identical (see `migrateLegacyOscLevels` / `PresetManager::load`).

**Oscillators & mixer (6A).** Three PolyBLEP oscillators, each with its own
level fader and a hardware-style **kill switch** (an off oscillator is skipped
entirely — measurable CPU savings, not just muted). Velocity routes to amplitude
(`vel_to_amp`) and filter cutoff (`vel_to_cutoff`) for dynamic playing.

**Presets (6D).** 16 read-only factory patches spanning Bass / Lead / Keys / Pad /
Pluck / Brass / Strings / Winds / Organ / FX, plus **Init**, in a category-grouped
Load menu. Factory patches are embedded JSON (override-on-Init); tweak-and-Save
makes a user copy. Details in [docs/presets.md](docs/presets.md).

**Plug-and-play MIDI (6C).** In the standalone, plugging a controller in mid-session
auto-connects it, applies its **device profile** (default CC map), and toasts;
unplugging releases held notes. Profiles are JSON (factory profiles embedded for
the Launchkey Mini and Korg B2; user overrides in the config dir), with precedence
**learned > user > factory**. Full details in [docs/midi.md](docs/midi.md).

**FX chain (6B).** A global, **reorderable** stereo chain of four hand-rolled,
JUCE-free effects (all in `Source/DSP/`, allocation-free after `prepare`): chorus,
ping-pong delay, Freeverb-style reverb, and mid/side stereo width. Each block has
a kill toggle (disabled ⇒ skipped, no CPU) and rotary params. Drag the blocks in
the far-right FX panel to reorder them; the audio chain crossfades to the new
order click-free (~30 ms) via a dual-chain equal-power blend. The order is a
`fx_order` **state-tree property** (a permutation, not an automatable value),
mirrored to a lock-free atomic for the audio thread and saved with presets.

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
| `Source/DSP/Chorus.h` `StereoDelay.h` `Reverb.h` `StereoWidth.h` | The four hand-rolled stereo FX. |
| `Source/DSP/FXChain.h` | Reorderable FX chain + click-free reorder crossfade. |
| `Source/MidiLearnManager.h` | (channel, CC) → parameter mapping, with learned/user/factory precedence. |
| `Source/MidiProfile.h` | JSON device-profile parsing + factory/user library (see `resources/midi-profiles/`). |
| `Source/PluginProcessor.*` | JUCE seam: MIDI dispatch, param snapshot, render, state + legacy migration. |
| `Source/PluginEditor.*` | Hardware-style custom editor: signal-flow panel sections, touch faders. |
| `Source/UI/FXPanel.h` | Far-right FX column: rotary blocks with finger drag-reorder. |
| `Source/PresetManager.h` | User preset save/load (per-user dir) + musical randomize; migrates legacy patches. |
| `Source/FactoryPresets.h` | Read-only factory presets (JSON in `resources/presets/`, embedded). |

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

## Custom UI

Hardware-style single-surface panel (`docs/editor.png`), left-to-right in signal
flow: **Osc 1 · Osc 2 · Mix · Filter · Amp Env · Filter Env · LFO · Global**.
Vertical faders + touch segmented buttons, each bound to its APVTS parameter via
attachments (GUI ↔ automation ↔ MIDI-learn all stay in sync), with a live value
readout. Dark hardware LookAndFeel; FlexBox layout scales with the window.

- **MIDI-learn**: right-click (mouse) or long-press (touch) any control → arms it
  (amber pulse); the next CC binds it and a `CCnn` badge appears. Same gesture
  clears a mapping. The 8 default Launchkey knobs show badges on first launch.
- **Presets** (Global section): **Random** shuffles every parameter (master gain
  kept audible) for sound exploration; **Save** stores the current settings by
  name; the **Load** dropdown recalls them (`~/.config/VASynth/presets/`).
- **Fullscreen**: F11 (standalone). **Debug overlay**: F12.
- QWERTY note input keeps working while twisting controls (controls refuse
  keyboard focus). VST3 uses the same editor, freely resizable.

## Observability (logging, health, debugging)

**Log file.** `~/.config/VASynth/VASynth.log` (JUCE default app-log location).
Each session appends a startup banner (version + git hash, build type, osc
quality, max voices, sample rate + buffer) and, from the standalone, the selected
audio device + type and enabled MIDI inputs (re-logged on device/MIDI changes).

**Audio-health stats.** Every ~10 s the log gets a line like:
```
render ms  min=0.05 med=0.37 p99=0.89 max=0.93 (16.7% budget)  voices<=6  steals=0  overruns=0  dropped=0  n=3750
```
`p99 % budget` is the headline CPU number; **overruns** (a callback exceeding the
buffer period) are logged immediately as an xrun early-warning. Logging is
real-time-safe: the audio thread only pushes POD events into a lock-free ring; a
background thread formats and writes them (drops + counts if the ring floods,
never blocks the audio thread).

**Debug overlay.** Press **F12** in the editor for a live overlay: CPU %, voice
high-water, steals, overruns, and the log-drop counter.

**Sanitizer builds.**
```
./run-all-checks.sh --sanitize      # ASan+LSan then UBSan: tests + memory soak
```
Or manually: `cmake -B build-asan -DVASYNTH_ASAN=ON -DVASYNTH_BUILD_TESTS=ON`
(also `-DVASYNTH_UBSAN=ON`). The soak (`tests/soak`) runs a MIDI storm through
`processBlock` and checks memory stability; under ASan, LeakSanitizer gates leaks.

**Reporting a bug — please send:**
1. `~/.config/VASynth/VASynth.log` (has the banner, health stats, any OVERRUN /
   CRASH markers, and the log-drop count).
2. What you were doing (patch, polyphony, which keyboard/controller).
3. Audio settings (device, sample rate, buffer) — visible in Options → Audio/MIDI
   Settings and logged in the banner.

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
