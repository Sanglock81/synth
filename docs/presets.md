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
| Drums   | Kick 808, Kick Punchy, Snare, Hat Closed, Hat Open, Tom |

### Drum recipes (7A)

The drums use the **Mod Env → Pitch** route (a filter/mod-envelope that also drives
pitch): an instant-attack env sweeps the pitch down over the hit, then the amp
decays percussively (sustain 0). Play them low (a kick sits about an octave below
the note). Recipes:

| Drum | Sound |
|---|---|
| Kick 808 | sine, +22 st pitch drop over ~55 ms, long 0.38 s boom, dry |
| Kick Punchy | tighter: +18 st over ~28 ms, short 0.16 s body |
| Snare | sine tone + noise through a bandpass, +7 st drop, ~150 ms, a touch of reverb |
| Hat Closed | noise only (oscs off), highpass ~8.5 kHz, ~45 ms |
| Hat Open | same, ~400 ms |
| Tom | sine, +7 st / ~80 ms drop, ~260 ms decay |

Full simultaneous **kits** arrive with 7C parts (pads on a locked drum part).

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

## Kits

A **Kit** is a special kind of part: a per-note map of up to **16 pads**, laid out as
a 4×4 grid (Launchkey style). Each pad has:

- a **trigger note** (which key/pad fires it),
- a **source preset** (baked per pad — any factory/user patch becomes that pad's sound),
- **sounding note(s)** — 1 note for a normal hit, or **2–4 for a chord pad** (a tuned
  stab from a single hit); the sounding pitch is *decoupled* from the trigger, so a pad
  can sound any pitch(es) regardless of which key triggered it,
- a **level**, and a **choke group** (0 = none).

**Choke semantics.** Hitting a pad in a nonzero choke group instantly (click-free, ~4 ms)
cuts any still-ringing pads in the *same* group — the classic closed-hat-silences-open-hat
behaviour. Re-hitting the *same* pad retriggers it (a monophonic pad). A trigger with no
pad mapped is silent. Note-off releases exactly the sounding notes that trigger fired
(chord pads included), even if you edited the kit while the pad was held.

**Editing.** Click a locked part cell (P1–P3) on the PARTS strip to open the **Kit
Editor**. Per pad: set the trigger and sounding notes by **learn-by-play** (arm, then
press keys), pick the source preset, set level and choke group, and **Audition**. Kits
save/load as their own presets (a **Kits** category) and are included in a **MULTI**.

**Factory kits.** *808 Basics* — six drums on triggers 36–41 (Kick 808, Kick Punchy,
Snare, Hat Closed, Hat Open, Tom), with the two hats in choke group 1. *Stab Board* —
four drums plus four tuned **minor-triad** chord pads (a plucky Synth Pluck at C/D/E/F).

**Kit seam note.** In this version every pad of a Kit part shares the part's one FX/LFO
chain (per-part FX arrives with the full-multitimbral work); a "drums" split zone plays
its Kit chromatically across the zone via the pads' trigger notes.

## Under the hood

Factory presets are JSON in `resources/presets/`, embedded via BinaryData. Each
lists parameter overrides in real units (Hz, seconds, cents, choice index) applied
on top of an Init baseline, plus an optional FX `fxOrder`. Adding a JSON there and
rebuilding is all it takes to ship another patch — the build globs the folder.

Kits are XML under the app's `kits/` folder (factory kits are built in). A kit lists its
pads (trigger, source preset, sounding notes, level, choke); each pad's source is baked
to `VoiceParams` on assignment, exactly like a locked part, so a pad sounds identical to
loading that patch live. The per-note `paramsFor(part, note)` engine seam selects the
right pad's params per voice.

## Velocity → filter (per-category convention)

Velocity always drives the VCA (`vel_to_amp`, level). Whether it *also* opens the
filter (`vel_to_cutoff`, brightness) is a **per-preset** choice — the global default
stays `0`, so user patches are never surprised. Factory patches follow one convention so
harder playing reads as more expressive, category by category:

| Category            | `vel_to_cutoff` | Why |
|---------------------|-----------------|-----|
| Keys / EP           | ~0.5            | Rhodes-style bark on hard hits is the defining EP behaviour |
| Leads / Basses / Plucks | ~0.4        | Bright, articulate attack that tracks how hard you play |
| Sub bass            | ~0.25           | Gentler — keep the low end solid under accents |
| Brass / Strings / Winds | ~0.3        | Moderate; brass especially brightens with force |
| Pads                | ~0.1–0.15       | Subtle shimmer, not a filter sweep |
| Organs              | 0               | Real organs have no velocity response — flat is authentic |
| Drums               | 0               | Velocity → level only; per-step sequencer velocity covers accent dynamics |
| Init / blank        | 0               | The blank slate stays predictable; opt in per patch |

**Sine-only voices are the exception.** A pure-sine patch (e.g. *Digital Bell*) has no
harmonics for the filter to reveal, so `vel_to_cutoff` is inert there — those patches use
`vel_to_amp` alone. `tests/plugin/test_preset_velocity.cpp` renders every factory patch at
a soft vs hard velocity and asserts the spectral centroid rises where the patch routes
velocity to the filter (and stays flat where it deliberately doesn't).
