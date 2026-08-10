# Presets

synth ships with 160+ read-only **factory presets** (including the classic-machine kit voices) plus **Init**, and you can
save your own. The Load menu (in the Global panel) groups everything by category.

## Loading

Open the **Load** menu and pick a patch. The menu is **collapsible** so it opens compact:

- **Init** — pinned at the top; resets every parameter to its default and the FX chain to
  chorus → delay → reverb → width.
- **Factory** patches, in a submenu per category in a fixed musical order (Bass, Lead, Keys, Pad,
  Pluck, Brass, Strings, Winds, Organ, FX, Experimental, Drums) and **alphabetical within each category**.
- **My Presets** — a submenu of your saved patches (only once you've saved one), grouped by the
  category you chose at save time (older presets read as "User"). Each patch is a small submenu
  with **Load / Rename… / Delete** — rename keeps its category and won't overwrite another patch;
  delete asks first. Factory patches are read-only.

Selecting a patch applies it immediately; the panel returns to "Load" so you can
pick the same one again. The per-part **Load synth patch** picker (on a P1–P4 cell) uses the
same grouping; **Load drum kit** lists **Classic Machines** / **Originals** / **User** kits with each kit's pad count.

## Factory presets

| Category | Presets |
|---|---|
| Bass    | Deep Sub, Reese Bass, Acid Bass, WT Growl Bass, Sub + Click, Reese Redux, Power Grind, Velvet Bass, Stab Bass, Cavern Bass, Power Grind 5th, Reese Sub |
| Lead    | Bright Lead, Foundry Lead, Dream Chime, PWM Anthem, Chip Lead, Fifth Stack, Supersaw, Screamer, Vowel Talk Lead, Glass Whistle, Soft Solo, Bright Lead 5th, Foundry 5th, Screamer 5th |
| Keys    | E-Piano, Digital Bell, EP Bark, Soft EP, Bell Keys, Clav Bite, Velvet Poly, Tape Keys, Toy Piano, Glass Harmonica |
| Pad     | Aurora Pad, Boreal, Choir Ahh, Submerged, Prairie Ensemble, Floating Poly, Anvil Choir, Warm Pad, Glass Pad, WT Vowel Pad, Motion Pad, WT Drift Pad, Dark Hollow, Anvil 5ths, Dark Hollow Sus, Warm Sus4 |
| Pluck   | Synth Pluck, Nylon Pluck, Music Box, Kalimba, Raindrop, Foundry Stomp, Ice Pluck, WT Marimba, Rubber Pluck |
| Brass   | Analog Brass, Dawn Brass, Ska Stab |
| Strings | String Machine, Strings Redux |
| Winds   | Soft Flute, Breath Flute, Tin Whistle, Ocarina |
| Organ   | Full Organ, Harmonium Reed, Cathedral Pipe, Percussive B |
| FX      | Dark Drone, Cave Drone, Static Riser, Ghost Sine, Sheet Metal Riser, Thunder Sheet, Radio Ghost |
| Experimental | Metal Ping, Pendulum, Feedback Bloom, Breathing Machine, One-Voice Choir, Gamelan Ghost, Gravity Well, Insect Swarm, Sputter |
| Drums   | Kick 808, Kick Punchy, Snare, Hat Closed, Hat Open, Tom, Clap, Rimshot, Clave, Cowbell, Splash, Crash, Ride, House Kick, House Snare, House Hat, Industrial Kick, Metal Hit, Noise Snare, Kick Studio, Kick Tight, Snare Studio, Sidestick, Clap Soft, Snare Brush, Hat Closed Soft, Hat Open Soft, Tom Studio, Shaker, Tambourine, Crash Dark, Ride Soft, Cowbell Low |

**Experimental** is its own category: wholly unique instruments — sounds that exist nowhere
else; play them to find out what they do. (Cave Drone and Static Riser stay in *FX/Texture* —
they read as production tools, an ambient bed and a riser, rather than play-to-discover voices.)

Patches can also carry **modulation routes** (see "Modulation routes in a patch" below), so a
sound's movement — an LFO morphing the wavetable vowel, an envelope opening a filter beyond its
static amount — travels with the preset. Showcases: *Vowel Talk Lead*, *Aurora Pad*, *Foundry Lead*, *Ghost Sine*, *Dream Chime* (LFO morphs the vowel table), *Pendulum* (a tempo-synced S&H LFO steps the cutoff — a clock-locked random melody), and *Feedback Bloom* (LFOs drift a self-oscillating filter).

### Drum recipes

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

Full simultaneous **kits** are locked drum parts (pads on a locked drum part).

Factory presets are **read-only** — embedded in the binary, never overwritten.
Tweak one and hit **Save** to keep your version as a user preset (a copy); the
factory patch is untouched.

## Saving your own

**Save** opens a modal name dialog with a **Category** picker (defaults to *User*; pick any
factory category to file the patch there). No text field lives on the main panel, so QWERTY
note input is never starved. Your presets are stored as XML here:

```
~/.config/synth/presets/*.vasynth      # Linux
```

Drop files in or remove them freely; they appear under **User** in the Load menu.
A user preset captures the full parameter state, the FX chain order, your **mod-matrix
routes**, and your learned MIDI mappings, so it recalls exactly.

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

**Factory kits.** Synthesized recreations inspired by classic drum machines, **voiced to sound
good first** — the documented tunings/circuits are the starting point, then we deviated wherever it
sounded better. All on triggers 36–51 (the Launchkey pad grid); hats choke (group 1), cymbals ring
free (group 0). Grouped in the picker as **Classic Machines** / **Originals** / **User**.

> **Disclaimer.** Synthesized recreations inspired by classic drum machines; model shorthand is used
> descriptively; no affiliation with or endorsement by any manufacturer.

*Classic Machines* (ten kits — the analog-heritage four + the PCM-homage six):

- **808** — deep sine kick with a long boom, two-tone snare, square-stack metallic hats, the
  540/800-flavour cowbell, toms + congas, clave/maraca, a long cymbal. The warm, round benchmark.
- **909** — punchy click-attack kick (harder pitch sweep), bright cracking snare, aggressive toms;
  hats/ride/crash/rim voiced **hotter and dirtier** (per-voice `filter_drive` grit) than the 808's.
- **606** — thin, sharp, cheap-and-lovely: clicky kick, biting snare, sizzly hats + cymbal; the sparse
  original filled to 16 with tuned variants.
- **78** — soft vintage preset-rhythm colours: gentle kick, brushy snare, warm hats, a metallic beat,
  guiro, bossa woodblocks, maraca, cowbell. Warm and lo-fi.
- **707** — bright, plasticky, precise: tight kick, papery snare, crisp hats, ride + crash.
- **LM1** — gated-real-drums feel: thuddy kick, fat cracking snare, prominent toms, warm-dark top;
  **no cymbals** (honoured) — filled with congas / claps / tambourine.
- **DMX** — harder, crunchier electro backbone: solid kick, big gated snare, bright claps
  (SAT-forward `filter_drive`).
- **RX5** — mid-80s digital sheen: sharp attacks, bright metallic hats, aggressive toms + rimshot.
- **R50** — crisp late-80s PCM: clean, punchy, slightly clinical top.
- **MP60** — boom-bap: deep rounded kick, dusty snare, dark hats, dark ride; **SAT-forward** warmth
  (`filter_drive`) is the point.

Neighbouring kits are intentionally tellable apart (808/DMX/MP60 kicks; the hats across 606/909/RX5).

*Originals* (house-designed, unchanged): **Industrial** (driven, metallic — distorted kick, harsh
noise snare, clanging metal-hit toms) and **Studio** (synthesized general-purpose, warm/round for
songwriter/pop/rock demos). *Fully-acoustic realism is the 1.1 sampled-kit pass; you can also load
your own samples onto any pad today.*

*808* pad map (pad = trigger − 36; the other kits follow the same kick/snare/hat conventions):

| Trig | Pad | Trig | Pad | Trig | Pad | Trig | Pad |
|------|-----|------|-----|------|-----|------|-----|
| 36 | Kick (≈45 Hz, ~0.55 s boom) | 40 | Clap | 44 | Low Tom¹ | 48 | Hi Conga¹ |
| 37 | Kick Tight | 41 | Cowbell | 45 | Mid Tom¹ | 49 | Cymbal² |
| 38 | Snare (two-tone + noise) | 42 | Hat Closed³ | 46 | High Tom¹ | 50 | Clave |
| 39 | Rim | 43 | Hat Open³ | 47 | Low Conga¹ | 51 | Maraca |

¹ one preset re-tuned via the pad's sound-note (no extra preset). ² the cymbal is **not** choked
(group 0) so it washes over the groove. ³ the two hats share choke group 1. Every pad responds to
velocity. The step sequencer's 8 default rows map to kick · snare · clap · closed-hat · open-hat ·
low-tom · cymbal · cowbell.

**Migration.** A MULTI or `.kit` that stored a retired kit loads its successor: *808 Basics* → **808**,
*House Basics* → **909**, *Stab Board* → **808** (the chord-pad *feature* stays — build one in the Kit
Editor with 2–4 sounding notes on a pad).

**Tour renders.** The verification suite renders a fixed one-bar reference pattern through every
classic kit to `docs/audio-refs/<kit>.wav` (local, not committed) so the whole library can be A/B'd
in one sitting.

**Kit seam note.** In this version every pad of a Kit part shares the part's one FX/LFO
chain (per-part FX arrives with the full-multitimbral work); a "drums" split zone plays
its Kit chromatically across the zone via the pads' trigger notes.

## Under the hood

Factory presets are JSON in `resources/presets/`, embedded via BinaryData. Each
lists parameter overrides in real units (Hz, seconds, cents, choice index) applied
on top of an Init baseline, plus an optional FX `fxOrder`. Adding a JSON there and
rebuilding is all it takes to ship another patch — the build globs the folder.

### Modulation routes in a patch

A patch can carry up to 8 **mod-matrix routes** — the same routing you make live with LINK —
so its movement recalls with the sound. In a factory JSON:

```json
"routes": [
  { "src": "LFO 1", "dest": "Wave Pos", "depth": 0.45 },
  { "src": "Velocity", "dest": "Reverb Mix", "depth": 0.3 }
]
```

- **`src`** — one of `LFO 1`/`2`/`3`, `Mod Env`, `Amp Env`, `Velocity`, `Note`, `Mod Wheel`,
  `Pitch Bend`, `Random`, `Macro 1`…`8` (exact display names).
- **`dest`** — any modulation destination's display name (`Cutoff`, `Resonance`, `Wave Pos`,
  `Reverb Motion`, `Delay Feedback`, an EQ band gain, …).
- **`depth`** — −1.0…+1.0.

Routes apply to the **focused part** on load; a patch that declares none clears the focused
part's routing (a patch is its complete sound). **User** patches round-trip their routes
automatically — saving captures the focused part's routes, loading reapplies them.
(Unknown source/dest names are skipped, never a bad slot. Full DAW/MULTI recall uses the
separate all-parts `mod_matrix` state.)

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
