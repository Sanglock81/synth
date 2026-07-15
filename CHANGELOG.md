# Changelog

All notable changes to **synth** are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project aims to
follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

Post-1.0 work on `master` (not yet tagged; the ThinkPad validation is the final pre-tag gate).

### Changed
- **Looper is now 4 fixed per-part lanes** (lane N ↔ part N), each with its own
  REC/PLAY/CLEAR/MIDI-AUDIO transport and its own audio ring. Capture is by lane (part N),
  fully decoupled from the edit/play focus (switching focus no longer disturbs the looper).
  Shared loop-grid; honest audio-bar cap at low tempo; per-lane UI rows. REC is one-shot:
  arm, engage at the loop downbeat, record exactly the set bars (1/2/4), then auto-stop and
  play.
- **Poly/Mono/Legato is now per-part** (edited via focus like the rest of the sound). Each
  part has its own mode, mono voice, note stack and glide; kit parts are always poly. Fixes a
  real isolation break — a mono lead on part 1 was cut whenever the sequencer ran on part 4
  (global single-voice mono). The master EQ / per-part EQ, filter, FX and LFOs are all per-part.
- **Per-part 3-band parametric EQ** as a 5th reorderable FX block; the master EQ stays a global
  finisher. Every FX/EQ on/off header is a loud lit toggle.
- **Stereo width now widens a dry mono source.** width > 1 synthesizes side content from
  the mid via a Schroeder allpass cascade (phase-only, not a Haas delay), added purely
  antisymmetrically so the mono fold-down is unchanged. width ≤ 1 and width == 1 unchanged.
- **Default scene:** the drum kit moved to **P4** (the sequencer's default target); P3 stays
  the bass, P2 is now the free spare.
- **808 / Punchy kick voicing:** amp attack softened 1 ms → 2 ms for a defined transient.

### Fixed / investigated
- Investigated a reported width/EQ "does nothing": both work in the real processor topology
  (added real-topology tests); width was a mono no-op (now fixed above), the master EQ works.
- Investigated a reported 808 kick "HF click/pop": the kick is **engine-clean** (measured far
  below the click standard, single hit and rapid retrigger; block-size-independent) — locked
  in by a regression. Remaining transient character is preset voicing.

## [1.0.0] — 2026-07-13

First public release: a JUCE 8 / C++17 virtual-analog polysynth (VST3 + Standalone,
Linux-first) built for a 2015 dual-core ThinkPad live target. Every change shipped
test-first behind a full gate (`run-all-checks.sh` + `--sanitize` + bench).

### Synth engine
- Three PolyBLEP oscillators (saw / square+PWM / triangle / sine), 4× oversampled with a
  configurable quality/CPU tradeoff (`Efficient` audible-band-clean default, `HQ` full-band
  for studio); per-source levels, octave, detune, noise, kill switches.
- Cytomic TPT state-variable filter (LP/HP/BP/Notch) with control-rate cutoff, key-track,
  and filter-envelope amount; dedicated amp + mod (filter) ADSRs; mod-env → pitch.
- Click-free voice stealing and retrigger (phase only resets on an idle voice); 24-voice
  pool with a fixed voice-sum headroom trim (goldens bit-identical across pool sizes).
- Mono / legato / poly modes with glide (portamento); pitch bend, mod-wheel vibrato, and
  sustain-pedal (CC64) handling from any device.
- Per-block parameter smoothing (cutoff/resonance/osc-mix + master gain) — no zipper noise.

### Multitimbral, kits, mixer
- Four parts: part 0 live + parts 1–3 locked, each with its own baked voice params, FX
  chain, and three LFOs (race-free lock-free publish). Silent parts with idle FX are skipped.
- Drum **kit parts**: per-pad synth voices, choke groups, note-off ledger, learn-by-play,
  per-pad editing (open the full synth panel on any pad), factory kits + user `.kit` files.
- Per-part **mixer** (level + 0 dB-centre balance pan), MIDI-learnable, captured in MULTI.
- **Full per-part isolation**: a generator (sequencer/arp/looper) always yields its voices to
  live playing, so a running pattern can never cut a note you play; editing one part (or one
  kit pad) never affects another.

### Performance surfaces
- Diatonic **chord engine** with momentary QWERTY / CC / consumed-note modifiers that
  re-voice even held chords (major/minor/sus/dim/7th/dom7).
- **Arpeggiator** + 8-row **step sequencer** (dedicated drum grid), clock-linked, targeting a
  single part.
- **Looper**: clock-linked, armed + measure-quantized, dual MIDI + AUDIO lanes, WAV export.
- Multi-surface **input routing** (QWERTY + per-MIDI-device → parts) with key-range split
  zones; plug-and-play MIDI hot-plug, device profiles, and precedence.

### Mix / master / modulation
- Master 4-band parametric EQ (low shelf, two bells, high shelf) at the end of the chain,
  default-flat bit-identical bypass.
- Eight **macros**, routable to any parameter and assigned by Random; Launchkey pots drive
  them out of the box.
- Spliced safety clipper and voice-sum gain staging — output never exceeds ±1.0.

### UI / UX
- Hardware-style custom editor: left part rail (P1–P4 + kit-pad seam), signal-flow centre,
  right oscilloscope + FFT (RT-safe SPSC tap), slim top bar, chord/rhythm/looper bottom
  zones, `?` help overlay, fullscreen (kiosk) mode. Touch-reliable (Surface-confirmed);
  nothing on the main panel steals keyboard focus.
- **Default startup scene**: P1 lead · P2 spare · P3 bass · P4 808 kit (the sequencer's
  target) — playable and audibly distinct out of the box.
- Edit-focus: tapping a part swaps the whole panel to its sound with per-part persistence,
  MULTI edit-capture, and revert. Double-tap numeric entry; LFO→knob modulation animation.
- Factory preset library (16 patches across categories) + Init; **loading a patch is
  sound-only** — it never disturbs the sequencer, looper, tempo, macros, mixer, or other parts.

### Persistence & recall
- APVTS state round-trip (incl. persisted MIDI-learn maps); a preset persists SOUND only.
- **MULTI** save/load is the one explicit recall of the full multitimbral layout (parts,
  locked presets, kits, routing, zones); routing/zones otherwise reset to the default scene
  on relaunch.

### Standalone / platform
- Standalone app with just-works PipeWire output, QWERTY keyboard input, and robust input
  reliability across focus/lifecycle events.
- Observability: RT-safe SPSC ring logger, audio-health telemetry, F12 debug overlay, crash
  handler, provenance banner.

### Engineering
- JUCE-free `Source/DSP/` (std-only), frozen parameter IDs, dumb voices (all APVTS access in
  a snapshot), audio thread free of allocation/locks/IO.
- Catch2 test suite (DSP + plugin-layer + RT-alloc guards + golden render + click-torture
  noise-cleanliness), pluginval strictness 8, ASan/LSan/UBSan + MIDI-storm soak, and a
  `dsp_bench` reporting block time against the ThinkPad budget. CI green on Linux + Windows.
- Licensed under **AGPLv3**.

### Known limitations / deferred to v2
- On-ThinkPad round-trip latency + voice-cap validation under PipeWire is the final
  pre-deploy check (`tools/thinkpad-validate/`); the dev-box ×3.5 derate flags the 24-voice
  pathological worst case, so the measured ThinkPad number is the arbiter for the voice cap.
- Not in v1 (planned for v2): mod matrix, MIDI-clock sync, unison/detune stacking, oscillator
  hard-sync / cross-mod, sub-oscillator, filter drive, and a preset browser.

[1.0.0]: https://github.com/Sanglock81/synth/releases/tag/v1.0.0
