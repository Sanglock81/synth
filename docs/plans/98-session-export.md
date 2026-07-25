# #98 — Session Export = DAW-handoff bounce (one folder)

> Committed design record. Scope confirmed by the user (a BOUNCE, not a portability bundle — the
> bundle is 1.1, task #116). Export-only. Offline, exact, fast.

## What it produces (one chosen folder)

- **Per-part WAV stems** — each part rendered OFFLINE through its full chain (voices → FX → EQ →
  level/pan). Source = the engine's per-part capture taps `capturePartL/R(p)` (the exact
  post-FX/post-pan contribution summed into the master), **pre-master-gain, pre-clip**.
- **Master WAV** — the summed mix through the master gain + `SoftClip` (post-clip).
- **Per-part MIDI** — each part's loop + step-seq content as a Standard MIDI File (reuse
  `exportLoopsToMidiFile`; extend it to also emit the step-seq grid as 16th-note note pairs).
- **manifest.json** — session name, ISO date, BPM, bar length (samples + seconds), sample rate,
  bars rendered, per-stem list (file → part name), app name+version+git hash, and a note that
  stems are pre-master-bus (pre-clip) while master.wav is post-clip.

## Render scope

- Length = the **current scene's realign cycle**: the longest active loop lane's bar count
  (all lane lengths are powers of two dividing 32, and the step-seq is 1 bar, so the longest lane is
  the common multiple). Default to that; a **bar-count override** field in the export dialog wins.
- **Offline** (message thread, not realtime): loop `processBlock` in bar-aligned chunks from a clean
  bar-1 clock, appending the master output + each part's `capturePart` tap into pre-sized buffers.

## Architecture (decisions)

- **Render on the LIVE processor**, audio device suspended by the export action (standalone), with a
  transport **reset → restore**: save `transportBeats` + `looper.position()` + the mix gain smoothers,
  reset to a bar-1 origin, render, then restore. Exact + includes all in-memory state; no state-copy
  fidelity risk. (Plugin/VST3 offline bounce is the host's job; this export is a standalone feature.)
- **Audio-loop *recordings* are NOT stemmed.** `capPart` is filled in `mixParts`, before the audio
  loop lanes sum into the master — so an AUDIO-mode lane is in the master but no stem. To keep
  **master == sum(stems)** for a clean handoff, audio-loop *playback* is disabled during the bounce.
  MIDI-loop + seq + scene content DOES render (it plays through the part voices). Documented; audio
  takes remain separately WAV-exportable. (Revisit if the user wants audio loops folded in.)
- **Determinism:** no host playhead → internal Tempo drives; `transportBeats` free-runs; wavetable
  random tables are seed-deterministic; nothing else in `processBlock` reads wall-clock or draws RNG.
  Two bounces of the same session are bit-identical (a test pins this).
- **MP3:** no in-tree encoder. If a system `lame`/`ffmpeg` is found, offer MP3 as an extra; never
  bundled, never a dependency. Deferred within #98 to the dialog increment (optional, best-effort).

## Increments (each: build + run-all-checks.sh ± --sanitize, goldens bit-identical)

### Inc 1 — the offline bounce core (audio + manifest)
- Processor `bounceSession(dir, bars, ...)`: reset transport, disable audio-loop playback, render
  `bars` offline capturing per-part stems (`capturePart`) + master; write `partN.wav` + `master.wav`
  (reuse the `exportLoopToWavFile` WavAudioFormat writer) + `manifest.json`; restore transport/flags.
- `realignBars()` helper (longest active lane). Tests (`tests/plugin/test_bounce.cpp`): correct file
  set + length (bars×barLen); **sum(stems) ≈ master** within clip tolerance on non-clipping material;
  a repeat bounce is bit-identical (determinism); goldens untouched (no default-path change).

### Inc 2 — per-part MIDI (SMF)  ← DONE
- `writePartMidiFile(dir/partN.mid, part, totalSamples, samplesPerBeat, bpm)`: the looper lane tiled
  to fill the cycle + (when this part is the seq target) the step-seq grid synthesized as 16th-note
  note-on/off pairs (`getSeqCell/getSeqStepVel/getSeqNote/getSeqMute`), each as a tempo-mapped SMF.
  Wired into `bounceSession` (a `"midi"` list in the manifest). Test: a 4-hit seq pattern yields
  exactly four note-ons in `part1.mid`. Included in the #98 Inc-1 gate cadence.

### Inc 3 — the export dialog (UI) + MP3
- A modal (OutputsDialog template): choose folder, bar-count override (default = realignBars()),
  optional MP3 toggle (enabled only if a system encoder is found). The standalone suspends audio
  around the bounce. Screenshot.

## Acceptance (UAT)
- The exported folder drags into Ableton AND Reaper and lines up at the manifest's BPM — the original
  acceptance test, verified in #115 UAT on real hardware.
