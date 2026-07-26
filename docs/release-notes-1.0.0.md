<!-- DRAFT — GitHub release body for v1.0.0. Edit to match the CHANGELOG reconciliation
     (docs/RELEASING.md Step 2) before publishing. Not auto-generated; keep it honest. -->

# synth 1.0.0

A virtual-analog polysynth for Linux and Windows — **standalone app + VST3 plugin**, built for
live performance as much as studio use. One instrument: four multitimbral parts, drums, a
performance looper, scenes, and a full modulation matrix, in a touch-reliable hardware-style UI.

## Highlights

**Synth voice**
- 4×-oversampled PolyBLEP oscillators (Efficient / HQ), a TPT state-variable filter with in-loop
  drive, dual ADSRs + mod-env→pitch, glide, and mono / legato / poly per part.
- **Wavetable** oscillator (5th wave) with position morph, a table picker, and a seeded randomizer.
- **Unison** — up to 7 stacked voices with non-uniform detune, stereo spread, and per-voice analog drift.
- Per-voice **analog drift** for a livelier, non-static tone.

**Multitimbral, drums, performance**
- **Four parts**, each with its own patch/kit, per-part FX (chorus, delay, motion reverb, width +
  velocity-sensitive saturation), three LFOs (free or tempo-synced), a 5-band EQ, and mixer level/pan.
- **Drum kits** — four factory kits (808 Basics, House Basics, Industrial, Stab Board), 16 pads each,
  per-pad editing, choke groups, and sample-pad loading.
- **Arpeggiator + step sequencer + a 4-lane looper** (MIDI or audio, up to 32 bars, WAV export) and
  **scenes** that swap drum patterns and loop clips on a bar boundary.
- Host-tempo follow, **MIDI clock out** (synth as master), and a one-folder **session BOUNCE** (per-part
  WAV stems + MIDI + manifest) that drops straight into Ableton or Reaper.

**Sound & control**
- **59 factory patches** across every category, **loudness-matched** via a per-patch TRIM control, in
  category-grouped Load/Save menus.
- A registry-driven **mod matrix** — any source to any continuous parameter — with touch-connect LINK.
- 8 assignable macros (Launchkey CC 21–28 out of the box), full **MIDI-learn** on every control
  (right-click / long-press), plug-and-play controller hot-plug, and QWERTY note input with Shift-octave.

**Standalone & platform**
- Curated PipeWire default on Linux (just-works audio), F11 fullscreen, an F12 live health overlay, a
  scope + FFT, and a build version/hash banner.
- Golden-render regression tests, ASan/UBSan + soak in CI, cross-platform state determinism.

## Install

See **[INSTALL.md](../blob/v1.0.0/docs/INSTALL.md)**. Quick path:

```bash
# Linux (Debian/Ubuntu)
./scripts/install-linux.sh
# Windows (PowerShell)
.\scripts\install-windows.ps1
```

Or grab the prebuilt package below and run its `install.sh` (Linux) / copy the VST3 (Windows).

## Known issues
- Intermittent (~3%) pluginval teardown crash on plugin unload — under investigation; does not affect
  normal DAW use.
- _(Add any UAT KNOWN-ISSUE items here.)_

## What's next (1.1)
Full **Sessions** (save & reopen a whole piece, recorded loops included), then the **Live Rig**
footswitch package (MIDI scene control, external-audio loop lanes, drum fills).

Full detail: [CHANGELOG.md](../blob/v1.0.0/CHANGELOG.md).
