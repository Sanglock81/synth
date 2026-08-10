# VA Synth

A hand-rolled virtual analog polysynth. C++17 / JUCE 8. Builds as a **VST3
plugin and a standalone app from the same code**, on Linux and Windows.

Designed for: a MIDI keyboard (primary) + Novation Launchkey Mini
(secondary keys + mappable CC controller) → your audio interface out.

## Architecture

```
                    ┌────────────────────────────────────────────┐
 MIDI in ──────────▶│ processBlock                               │
 (keyboard +        │   ├─ note on/off ──▶ SynthEngine           │
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
lowest latency into your audio interface, load the VST3 in a DAW (ASIO) or add
ASIO SDK support later.

### First run (standalone, Linux)

1. Launch the standalone. Open **Options → Audio/MIDI Settings**.
2. Output device: your audio interface (via ALSA, JACK, or PipeWire's JACK layer).
   Buffer 128–256 @ 48 kHz to start.
3. Enable **both** MIDI inputs: your keyboard and the Launchkey Mini.
4. Play the keyboard; twist Launchkey knobs (CC 21–28 pre-mapped: cutoff, reso,
   osc mix, env amt, attack, release, LFO rate, LFO depth).

## Roadmap

**v1 (make it sound good)**
- [ ] Verify PolyBLEP output against a reference synth by ear and spectrogram
- [ ] Pitch bend + mod wheel + sustain pedal handling
- [ ] Mono/legato modes + glide (slew note frequency in `SynthVoice`)
- [ ] Smooth per-block parameter changes (one-pole smoothing on cutoff/gain
      to kill zipper noise)
- [ ] Persist MIDI-learn mappings in APVTS state

**v2 (make it deep)**
- [ ] Mod matrix (any source → any destination, replaces single LFO dest)
- [ ] Second LFO, per-voice LFO option, MIDI-clock sync
- [ ] Unison/detune stacking, hard sync osc2→osc1
- [ ] Sub-oscillator, filter drive/saturation
- [ ] Custom GUI with right-click MIDI-learn
- [ ] Preset browser

**Reference reading (open-source virtual-analog projects worth studying)**
- Small, readable GPL softsynths — closest architectural siblings
- Open-source filter-algorithm libraries (SVF / ladder / diode models, MSEG shapers)
- The framework's own plugin examples — processor/editor patterns
