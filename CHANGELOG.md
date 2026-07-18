# Changelog

All notable changes to **synth** are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project aims to
follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

Post-1.0 work on `master` (not yet tagged; the ThinkPad validation is the final pre-tag gate).

### Changed
- **The per-part EQ reaches drum-kit parts, and kit parts are now focusable (K2).** Tapping any
  part cell — synth **or** kit — moves the shared edit-focus, so the EQ section (and the part's
  channel) follows it. A kit's summed output now runs through its per-part EQ (the creative FX —
  chorus/delay/reverb/width — stay dry on kits, only the EQ passes through). Because a drum kit has
  no single synth sound, while a kit is focused the OSC / FILTER / ENVELOPE / LFO panels dim with a
  "KIT — edit pads in Kit Editor" hint and a one-tap button; the kit keeps playing from its per-pad
  voices. Each part remembers its own EQ across focus changes and in the session. *(Previously the
  whole FX chain was bypassed for kits, and tapping a kit refused edit-focus — so its EQ was
  unreachable and the FFT showed no EQ shaping on drums. The master scope/FFT was already post-EQ.)*
- **One EQ, per part, at the end of the chain (K1).** The plugin now has a single EQ concept:
  a fixed **5-band parametric EQ** applied as the **last stage of the focused part's chain**
  (post-FX), living in its own right-column section that **follows edit focus** (the header
  names the part). It replaces two older, overlapping EQs — the **master finisher EQ is
  retired** (its `eq_*` params stay registered but are inert/hidden for state back-compat), and
  the per-part EQ is **removed from the FX drag-reorder chain** (that chain is now the four
  reorderable FX: chorus/delay/reverb/width). The new section is a mixing-desk surface: a
  vertical **gain slider per band** (drag up/down = gain, **drag sideways = frequency** with a
  live readout, **double-tap = numeric freq/gain/Q**), a per-band on/off dot, and a section
  on/off bar; editing any band auto-enables the section so a boosted band is never silently
  bypassed. The four band gains (`EQ Low/L-Mid/H-Mid/High Gain`) are mod-matrix / macro targets.
  Old presets migrate transparently: an EQ anywhere in a saved `fx_order` now always runs last
  (its slot is inert), and band 4 + the per-band switches default to a neutral, on state.
  *Fixes a latent bug:* the per-part EQ previously did nothing on the LIVE/focused part
  (`snapshotFXParams` never carried it) — it now works on every part.
- **Macros ship pre-assigned.** The 8 macros now default to musical targets — M1 filter
  cutoff, M2 resonance, M3 filter-env amount, M4 amp release, M5 LFO1 rate, M6 LFO1 depth,
  M7 reverb mix, and M8 the **focused part's level** (follows the edit focus). Older sessions
  with no saved macro map inherit these defaults; an explicitly-cleared map is respected. The
  Launchkey Mini pots (CC 21–28) already drive M1–M8, so the controller is expressive out of
  the box.
- **Per-step velocity on both rhythm surfaces.** Every sequencer step AND every arpeggiator
  step carries its own velocity percentage (10–200 %), edited with one grammar on both grids:
  **single-tap a dark box turns it on; double-tap a lit box turns it off** (a stray tap never
  silences a step); **touch-and-hold a box then drag up/down sets its velocity** — shown as a
  number in the box while adjusting and a bottom-up fill (accented, > 100 %, brightens) at
  rest. On the arp the velocity belongs to the STEP, not the note — the same box scales
  whatever note the pattern lands on it. Replaces the old binary sequencer accent (legacy
  accented steps migrate to a high velocity). Persists with patterns/presets/MULTIs; states
  without per-step velocities load at 100 %. (The brief single arp-velocity knob added earlier
  in this cycle is retired in favour of per-step control.)
- **Clock alignment:** the sequencer, arpeggiator and looper now share one transport origin
  (the loop clock), re-anchoring to the bar downbeat every bar — the seq no longer leads the
  arp/looper. Swing self-accumulates within the bar. Looper MIDI recording is quantized to a
  1/32 grid (per-lane toggle, default on) with note-pairing protection.
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
- **A matched controller profile is now authoritative.** Plugging in a Launchkey re-asserts
  its 8 pots (CC 21–28) onto the 8 macros on connect, overriding any stale/learned binding an
  old session left on those CCs — so the pots always drive the macros with no manual step. The
  match is broadened to any "Launchkey" (Mini/MK3/25/…); learn still wins live until the next
  hot-plug. (Reset MIDI + macros remains as a manual factory-restore.)
- **Standalone launches full-screen** by default (the recommended live mode — no OS title bar
  for a touch drag to catch); the maximise button still toggles back to a window.
- **Reset MIDI + macros, and touch-friendly macro knobs.** Added a **Reset MIDI + macros**
  button (INPUTS dialog) that restores BOTH the controller map (CC 21–28 → the 8 macros,
  clearing any learned/stale binding) AND the macro→target assignments (M1 cutoff … M8
  focused-part level) — recovers the Launchkey pots and the macro destinations when a past
  session had them pointing elsewhere. The top-bar macro knobs now also accept **horizontal**
  drag (not just vertical), so on a windowed touch screen you can adjust them sideways instead
  of dragging up into the OS title bar (which would grab the drag and move the window).
- **Per-step velocity now audibly shapes the note.** The seq/arp emit was clamping velocity
  to `min(1.0, …)`, so a step at 100 % already emitted the maximum and the whole 100–200 %
  "accent" range was inert. Velocity is now a real `0.1–2.0` scalar (100 % unchanged; > 100 %
  boosts, < 100 % ghosts). It reaches the voice and drives BOTH the VCA (`vel→amp`) and the
  filter (`vel→cutoff`) — louder *and* brighter on a harder step — verified end-to-end. The
  output safety clipper still guarantees the bus never exceeds ±1.0 on an over-unity accent.
  (Brightness response is per-preset via `vel_to_cutoff`; amplitude response is on by default.)
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
