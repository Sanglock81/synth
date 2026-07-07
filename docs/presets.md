# Presets

synth ships with 16 read-only **factory presets** plus **Init**, and you can
save your own. The Load menu (in the Global panel) groups everything by category.

## Loading

Open the **Load** menu and pick a patch. Entries are grouped:

- **Init** — resets every parameter to its default and the FX chain to
  chorus → delay → reverb → width.
- **Factory** patches, grouped by category (below).
- **User** — your saved patches (only shown once you've saved one).

Selecting a patch applies it immediately; the panel returns to "Load" so you can
pick the same one again.

## Factory presets

| Category | Presets |
|---|---|
| Bass    | Fat Saw Bass, Deep Sub, Reese Bass |
| Lead    | Bright Lead, Square Lead |
| Keys    | E-Piano, Digital Bell |
| Pad     | Warm Pad, Glass Pad |
| Pluck   | Synth Pluck |
| Brass   | Analog Brass |
| Strings | String Machine |
| Winds   | Soft Flute |
| Organ   | Full Organ |
| FX      | Noise Riser, Dark Drone |

Factory presets are **read-only** — embedded in the binary, never overwritten.
Tweak one and hit **Save** to keep your version as a user preset (a copy); the
factory patch is untouched.

## Saving your own

**Save** opens a modal name dialog (no text field lives on the main panel, so
QWERTY note input is never starved). Your presets are stored as XML here:

```
~/.config/synth/presets/*.vasynth      # Linux
```

Drop files in or remove them freely; they appear under **User** in the Load menu.
A user preset captures the full parameter state, the FX chain order, and your
learned MIDI mappings, so it recalls exactly.

## Master gain is yours, not the preset's

**Loading any preset — Init, factory, or user — never changes the MASTER level.**
Master gain is a global *performance* control: you set your output level for the
room/rig once, and switching patches leaves it exactly where it is (the same reason
Randomize never touches it). Saved user presets don't store a master value at all.
The single exclusion list lives in `PresetPolicy::excludedParams()` (`Parameters.h`).

## Loudness

Factory patches are **level-matched**: played as a single sustained note they sit
within a few dB of each other (~−33 dBFS RMS), so switching patches doesn't jump the
volume. Matching is done by trimming a patch's internal oscillator levels (never the
master). **Percussive/evolving patches are deliberately not RMS-matched** — a Pluck,
Bell, or a slow FX Riser has most of its energy in a short transient or a late swell,
so integrated RMS understates them; they are matched by feel/peak instead. Drum
presets (7A) are likewise matched by transient, not sustain.

## Under the hood

Factory presets are JSON in `resources/presets/`, embedded via BinaryData. Each
lists parameter overrides in real units (Hz, seconds, cents, choice index) applied
on top of an Init baseline, plus an optional FX `fxOrder`. Adding a JSON there and
rebuilding is all it takes to ship another patch — the build globs the folder.
