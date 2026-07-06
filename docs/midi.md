# MIDI: plug-and-play, device profiles, and MIDI-learn

VA Synth is built to "just work" when you plug a controller in. In the standalone
app, MIDI is **plug-and-play**: connect a keyboard mid-session and it auto-enables,
its default control map loads, and a toast confirms it; unplug it and any held note
is released so nothing hangs.

## Hot-plug (standalone)

The standalone watches the system MIDI device list (`MidiDeviceListConnection`).
When a device appears it is enabled as an input, its **device profile** is applied,
and a toast shows `"<name> connected"`. When a device disappears, an all-notes-off
panic runs (RT-safe) and a `"<name> disconnected"` toast shows. Devices already
present at launch have their profiles applied silently (no toast).

As a VST3 in a DAW, the host owns MIDI routing, so there's no hot-plug there — but
the same profiles still provide the built-in default control map.

## Device profiles

A profile maps a controller to sensible default **CC → parameter** bindings, so a
known device is useful the instant it's plugged in — no manual learning required.

Profiles are small JSON files:

```json
{
  "name": "Novation Launchkey Mini",
  "match": ["Launchkey Mini", "Launchkey Mini MK3"],
  "pitchBendRange": 2,
  "mappings": [
    { "cc": 21, "param": "filter_cutoff" },
    { "cc": 22, "param": "filter_reso" }
  ]
}
```

- `match` — a device matches when any string is a case-insensitive **substring**
  of the device name.
- `pitchBendRange` — pitch-wheel range in semitones (± ).
- `mappings` — CC → parameter-ID pairs (IDs are the stable strings in
  `Source/Parameters.h`).

**Factory** profiles are embedded in the binary (`resources/midi-profiles/`):
Novation Launchkey Mini and Korg B2. **User** overrides are JSON files you drop in:

```
~/.local/share/VASynth/midi-profiles/*.json      # Linux
```

## Precedence: learned > user > factory

Each CC mapping carries a source. When a profile is applied it never clobbers a
higher-precedence mapping:

- **Learned** (you MIDI-learned it live) — always wins, and persists in the patch
  state.
- **User** (your JSON override) — beats factory.
- **Factory** (built-in / embedded profile) — the baseline.

So a factory profile fills in defaults, your user JSON tailors a device, and any
control you learn by hand stays put regardless of what gets plugged in.

## MIDI-learn

Right-click (or long-press on a touchscreen) any control to arm learn (pulsing
amber outline); the next incoming CC binds it, shown as a `CCnn` badge. The same
gesture offers *clear mapping*. Learned mappings save with the patch/preset.
