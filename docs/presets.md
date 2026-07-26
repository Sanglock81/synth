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
| Bass    | Fat Saw Bass, Deep Sub, Reese Bass, Acid Bass, WT Growl Bass, Sub + Click, Unison Wide Bass, Reese Redux |
| Lead    | Bright Lead, Square Lead, WT Digital Lead, Supersaw, Screamer, Vowel Talk Lead, Glass Whistle, Soft Solo, Supersaw Slim |
| Keys    | E-Piano, Digital Bell, EP Bark, Soft EP, Bell Keys, Clav Bite |
| Pad     | Warm Pad, Glass Pad, WT Vowel Pad, Motion Pad, WT Drift Pad, Dark Hollow, Shimmer Bed |
| Pluck   | Synth Pluck, Ice Pluck, WT Marimba, Rubber Pluck |
| Brass   | Analog Brass, Solo Brass |
| Strings | String Machine, Strings Redux |
| Winds   | Soft Flute, Breath Flute |
| Organ   | Full Organ |
| FX      | Noise Riser, Dark Drone, Cave Drone, Static Riser, Metal Ping |
| Drums   | Kick 808, Kick Punchy, Snare, Hat Closed, Hat Open, Tom, Clap, Rimshot, Clave, Cowbell, Splash, Crash, Ride |

Some concepts (an envelope sweeping the wavetable position, a per-step timbre morph) want a
modulation route that the current preset format can't spell; those patches are voiced as close
as the fixed routes allow, and the **full versions are pending a preset-format extension** (1.1).

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

Every patch carries its own **program level** — a `patch_trim` parameter shown as the **TRIM**
knob in the top bar (right of the unison controls). Trim is a transparent post-FX gain baked
**with the sound** (saved in the preset, unlike MASTER which is yours), so a patch knows how loud
it wants to be. It is the lever the factory bank is level-matched with — no oscillator re-voicing,
no character change: the same patch, at a chosen level.

The **sustained, full-spectrum** factory patches are matched to within **±4 dB** of the bank
median (~−30 dBFS RMS on a held note), so switching patches during a set doesn't jump the volume.
`tests/plugin/test_preset_loudness.cpp` renders every patch and enforces this **post-trim**.

**Percussive/evolving patches are deliberately outside the RMS match** — a Pluck, Bell, EP, drum,
slow FX Riser or drone has its energy in a short transient or a late swell, so integrated RMS
understates them; they are matched by feel/peak instead (their TRIM sits at unity, free for you to
nudge). The check classifies a patch as *sustained* only when its amp envelope holds (sustain > 0.25)
**and** it is still ringing at full level late in the note — everything else is feel-matched.

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

**Factory kits.** *808 Basics* — a full **16-pad** kit on triggers 36–51 (the Launchkey
pad grid); the two hats choke each other (group 1), the cymbals ring free. *Stab Board* —
four drums plus four tuned **minor-triad** chord pads (a plucky Synth Pluck at C/D/E/F).

*808 Basics* pad map (pad = trigger − 36):

| Trig | Pad | Trig | Pad | Trig | Pad | Trig | Pad |
|------|-----|------|-----|------|-----|------|-----|
| 36 | Kick 808 | 40 | Clap | 44 | Low Tom¹ | 48 | Cowbell |
| 37 | Kick Punchy | 41 | Snare 2¹ | 45 | Mid Tom¹ | 49 | Crash² |
| 38 | Snare | 42 | Hat Closed³ | 46 | High Tom¹ | 50 | Clave |
| 39 | Rimshot | 43 | Hat Open³ | 47 | Splash² | 51 | Ride² |

¹ the Snare / Tom preset re-tuned via the pad's sound-note (no extra preset). ² cymbals
are **not** choked (group 0) so they wash over the groove. ³ the two hats share choke
group 1. The step sequencer's 8 default rows map to kick · snare · clap · closed-hat ·
open-hat · low-tom · crash · cowbell (trigger notes 36 · 38 · 40 · 42 · 43 · 44 · 49 · 48).

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
