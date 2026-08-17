#!/usr/bin/env python3
"""Generator for the synthesized classic-machine drum presets (kit building blocks).

Kits render DRY (no FX block), so grit comes from filter_drive (per-voice), never fx_sat/reverb.
Character over accuracy: documented tunings are the starting point; voiced to sound good.
Emits resources/presets/<NNN>_<slug>.json. Run from repo root: python3 tools/gen_drumkits.py
Idempotent by preset NAME; re-run after editing the spec. Increment 1 = 808 / 909 / 606 / 78.
"""
import json, os, re, math

OUT = os.path.join(os.path.dirname(__file__), "..", "resources", "presets")

# Drum synthesis defaults (a dry, one-shot percussive voice).
BASE = dict(osc1_wave=3, osc2_on=0, osc2_level=0, osc3_on=0, osc3_level=0, noise_level=0,
            filter_type=0, filter_cutoff=1000, filter_reso=0.0, filter_drive=0.0,
            filter_env_amt=0.0, fltenv_to_pitch=0,
            flt_attack=0.001, flt_decay=0.05, flt_sustain=0.0, flt_release=0.04,
            amp_attack=0.001, amp_decay=0.2, amp_sustain=0.0, amp_release=0.05, vel_to_amp=0.5)

def d(**kw):
    p = dict(BASE); p.update(kw); return p

# ----- helper builders (character knobs are what differentiate the machines) -----
def kick(cut, sweep, fdec, adec, oct=-1, drive=0.0, noise=0.0, vel=0.4):
    return d(osc1_octave=oct, filter_cutoff=cut, fltenv_to_pitch=sweep, flt_decay=fdec,
             amp_decay=adec, filter_drive=drive, noise_level=noise, vel_to_amp=vel)
def snare(tone, noise, cut, adec, reso=0.3, drive=0.0, o2=None, sweep=6, vel=0.5):
    p = d(osc1_level=tone, noise_level=noise, filter_type=2, filter_cutoff=cut, filter_reso=reso,
          fltenv_to_pitch=sweep, flt_decay=max(0.02, adec*0.5), amp_decay=adec, filter_drive=drive, vel_to_amp=vel)
    if o2 is not None:
        p.update(osc2_on=1, osc2_wave=3, osc2_octave=1, osc2_detune=o2, osc2_level=tone*0.7)
    return p
def hat_noise(cut, adec, reso=0.2, drive=0.0, vel=0.4):
    return d(osc1_on=0, osc1_level=0, noise_level=0.85, filter_type=1, filter_cutoff=cut,
             filter_reso=reso, flt_decay=min(0.04, adec), amp_decay=adec, filter_drive=drive, vel_to_amp=vel)
def hat_metal(cut, adec, drive=0.0, vel=0.4):
    # square-stack metallic hat (3 detuned squares) + a little noise, high-pass, choked short/long.
    return d(osc1_wave=1, osc1_octave=2, osc1_level=0.35,
             osc2_on=1, osc2_wave=1, osc2_octave=2, osc2_detune=37, osc2_level=0.3,
             osc3_on=1, osc3_wave=1, osc3_octave=2, osc3_detune=-53, osc3_level=0.3,
             noise_level=0.25, filter_type=1, filter_cutoff=cut, filter_reso=0.15,
             flt_decay=min(0.04, adec), amp_decay=adec, filter_drive=drive, vel_to_amp=vel)
def tom(cut, sweep, adec, oct=0, drive=0.0, vel=0.5):
    return d(osc1_octave=oct, filter_cutoff=cut, fltenv_to_pitch=sweep, flt_decay=0.07,
             amp_decay=adec, filter_drive=drive, vel_to_amp=vel)
def clap(cut, adec, reso=0.4, drive=0.0):
    return d(osc1_on=0, osc1_level=0, noise_level=0.9, filter_type=2, filter_cutoff=cut,
             filter_reso=reso, flt_decay=0.09, amp_decay=adec, filter_drive=drive, vel_to_amp=0.5)
def cymbal(cut, adec, drive=0.0, noise=0.85):
    return d(osc1_wave=1, osc1_octave=2, osc1_level=0.2,
             osc2_on=1, osc2_wave=1, osc2_octave=2, osc2_detune=63, osc2_level=0.2,
             osc3_on=1, osc3_wave=1, osc3_octave=2, osc3_detune=-88, osc3_level=0.2,
             noise_level=noise, filter_type=1, filter_cutoff=cut, filter_reso=0.12,
             flt_decay=0.1, flt_sustain=1.0, flt_release=0.1,
             amp_decay=adec, amp_release=adec*0.4, filter_drive=drive, vel_to_amp=0.5)
def perc_sine(oct, cut, adec, sweep=0, vel=0.4):   # clave / conga / rim tone
    return d(osc1_octave=oct, filter_cutoff=cut, fltenv_to_pitch=sweep, flt_decay=0.03,
             amp_decay=adec, vel_to_amp=vel)
def noise_perc(cut, adec, reso=0.1, hp=True, vel=0.4):  # maraca / shaker / guiro
    return d(osc1_on=0, osc1_level=0, noise_level=0.8, filter_type=(1 if hp else 2),
             filter_cutoff=cut, filter_reso=reso, flt_decay=min(0.03, adec), amp_decay=adec, vel_to_amp=vel)
def cowbell(f1oct, det, cut, adec, drive=0.0):   # two-square cowbell (540/800 flavour)
    return d(osc1_wave=1, osc1_octave=f1oct, osc1_level=0.5,
             osc2_on=1, osc2_wave=1, osc2_octave=f1oct, osc2_detune=det, osc2_level=0.5,
             filter_type=2, filter_cutoff=cut, filter_reso=0.4, flt_decay=0.1,
             amp_decay=adec, filter_drive=drive, vel_to_amp=0.5)
def rim(cut, adec, noise=0.28, drive=0.0):
    return d(osc1_wave=2, osc1_octave=1, osc1_level=0.6, noise_level=noise, filter_type=2,
             filter_cutoff=cut, filter_reso=0.3, fltenv_to_pitch=8, flt_decay=0.02,
             amp_decay=adec, filter_drive=drive, vel_to_amp=0.5)

# ============================ Increment 1 presets ============================
# name -> params. Kit pad maps live in factoryKit() (C++); toms/congas re-tuned via tuned().
PRESETS = {
  # ---- 808: deep sine kick + long decay, square-stack metal hats, 540/800 cowbell ----
  "808 Kick":     kick(cut=480, sweep=20, fdec=0.05, adec=0.55, oct=-1, vel=0.4),
  "808 Kick Tight": kick(cut=520, sweep=20, fdec=0.045, adec=0.28, oct=-1, vel=0.45),
  "808 Snare":    snare(tone=0.5, noise=0.6, cut=1800, adec=0.16, o2=+12, sweep=6),   # two-tone + snappy noise
  "808 Rim":      rim(cut=2400, adec=0.05),
  "808 Clap":     clap(cut=1200, adec=0.11),
  "808 Hat Cl":   hat_metal(cut=9000, adec=0.045),
  "808 Hat Op":   hat_metal(cut=9000, adec=0.42),
  "808 Tom":      tom(cut=900, sweep=8, adec=0.28),
  "808 Conga":    perc_sine(oct=1, cut=1400, adec=0.18, sweep=6, vel=0.5),
  "808 Cowbell":  cowbell(f1oct=1, det=31, cut=1500, adec=0.24),
  "808 Clave":    perc_sine(oct=2, cut=4200, adec=0.06),
  "808 Maraca":   noise_perc(cut=10000, adec=0.03),
  "808 Cymbal":   cymbal(cut=6500, adec=2.6),

  # ---- 909: punchy click-attack kick (harder sweep), bright cracking snare, hotter/dirtier hats+cymbals ----
  "909 Kick":     kick(cut=3200, sweep=27, fdec=0.02, adec=0.26, oct=-1, noise=0.18, vel=0.35),
  "909 Snare":    snare(tone=0.55, noise=0.85, cut=2400, adec=0.13, reso=0.3, drive=0.25, o2=+7, sweep=7),
  "909 Rim":      rim(cut=2800, adec=0.045, noise=0.35, drive=0.2),
  "909 Clap":     clap(cut=1500, adec=0.12, drive=0.15),
  "909 Hat Cl":   hat_noise(cut=11000, adec=0.035, reso=0.25, drive=0.2),
  "909 Hat Op":   hat_noise(cut=11000, adec=0.42, reso=0.25, drive=0.2),
  "909 Tom":      tom(cut=1400, sweep=9, adec=0.24, drive=0.2, vel=0.5),
  "909 Ride":     cymbal(cut=7800, adec=0.9, drive=0.15, noise=0.5),
  "909 Crash":    cymbal(cut=7000, adec=3.4, drive=0.15),
  "909 Clave":    perc_sine(oct=2, cut=4200, adec=0.06),

  # ---- 606: thin, sharp, clicky ----
  "606 Kick":     kick(cut=2000, sweep=22, fdec=0.02, adec=0.18, oct=0, vel=0.4),
  "606 Snare":    snare(tone=0.45, noise=0.75, cut=2800, adec=0.10, reso=0.35, sweep=8),   # biting
  "606 Hat Cl":   hat_noise(cut=12000, adec=0.03, reso=0.3),
  "606 Hat Op":   hat_noise(cut=12000, adec=0.3, reso=0.3),
  "606 Cymbal":   cymbal(cut=8500, adec=1.6, noise=0.7),
  "606 Tom":      tom(cut=1200, sweep=8, adec=0.20, vel=0.5),

  # ---- 78: soft vintage preset-rhythm colours (gentle, brushy, warm/lo-fi) ----
  "78 Kick":      kick(cut=400, sweep=14, fdec=0.05, adec=0.34, oct=-1, vel=0.35),
  "78 Snare":     snare(tone=0.3, noise=0.7, cut=1500, adec=0.13, reso=0.2, sweep=5),        # brushy
  "78 Hat":       hat_noise(cut=7000, adec=0.05, reso=0.15),
  "78 Beat":      d(osc1_wave=1, osc1_octave=1, osc1_level=0.4, osc2_on=1, osc2_wave=1, osc2_octave=1,
                    osc2_detune=24, osc2_level=0.4, filter_type=2, filter_cutoff=1800, filter_reso=0.35,
                    flt_decay=0.06, amp_decay=0.12, vel_to_amp=0.5),                          # metallic beat
  "78 Guiro":     noise_perc(cut=3000, adec=0.09, reso=0.3, hp=False),
  "78 Block":     perc_sine(oct=2, cut=3000, adec=0.07, vel=0.5),                             # bossa woodblock
  "78 Maraca":    noise_perc(cut=9000, adec=0.035),
  "78 Cowbell":   cowbell(f1oct=1, det=28, cut=1300, adec=0.2),
}

# ============================ Increment 2 presets (PCM-homage six) ============================
# Shared language: punchier envelopes, band-limited "cheap converter" brightness shaping, gated-
# feeling decays -- then voiced to sound GOOD. SAT-forward kits (DMX, MP60) lean on filter_drive.
PRESETS2 = {
  # ---- 707: bright, plasticky, precise ----
  "707 Kick":     kick(cut=1400, sweep=18, fdec=0.02, adec=0.18, oct=-1, noise=0.1, vel=0.35),
  "707 Snare":    snare(tone=0.4, noise=0.8, cut=2600, adec=0.11, reso=0.25, sweep=6),   # papery
  "707 Rim":      rim(cut=2800, adec=0.04),
  "707 Clap":     clap(cut=1600, adec=0.10),
  "707 Hat Cl":   hat_noise(cut=11500, adec=0.03, reso=0.2),
  "707 Hat Op":   hat_noise(cut=11500, adec=0.34, reso=0.2),
  "707 Tom":      tom(cut=1300, sweep=8, adec=0.22, vel=0.5),
  "707 Ride":     cymbal(cut=8000, adec=0.85, noise=0.55),
  "707 Crash":    cymbal(cut=7200, adec=2.8),
  "707 Cowbell":  cowbell(f1oct=1, det=30, cut=1600, adec=0.2),

  # ---- LM1: gated real-drums feel; thuddy kick, fat cracking snare, prominent toms; NO cymbals ----
  "LM1 Kick":     kick(cut=900, sweep=12, fdec=0.04, adec=0.24, oct=-1, vel=0.5),          # thuddy
  "LM1 Snare":    snare(tone=0.5, noise=0.8, cut=2000, adec=0.16, reso=0.25, drive=0.15, o2=+7, sweep=5),  # fat crack
  "LM1 Rim":      rim(cut=2200, adec=0.05),
  "LM1 Clap":     clap(cut=1300, adec=0.13),
  "LM1 Hat Cl":   hat_noise(cut=8000, adec=0.04, reso=0.15),                                # warm-dark top
  "LM1 Hat Op":   hat_noise(cut=8000, adec=0.36, reso=0.15),
  "LM1 Tom":      tom(cut=1100, sweep=9, adec=0.34, drive=0.1, vel=0.6),                     # prominent
  "LM1 Conga":    perc_sine(oct=1, cut=1300, adec=0.2, sweep=5, vel=0.5),
  "LM1 Tamb":     noise_perc(cut=9000, adec=0.06, reso=0.2),

  # ---- DMX: harder, crunchier electro backbone; big gated snare, solid kick, bright claps ----
  "DMX Kick":     kick(cut=1600, sweep=20, fdec=0.02, adec=0.22, oct=-1, drive=0.3, noise=0.1, vel=0.4),
  "DMX Snare":    snare(tone=0.5, noise=0.85, cut=2200, adec=0.17, reso=0.3, drive=0.4, o2=+7, sweep=6),  # big gated
  "DMX Rim":      rim(cut=2600, adec=0.05, drive=0.25),
  "DMX Clap":     clap(cut=1700, adec=0.13, drive=0.25),                                     # bright
  "DMX Hat Cl":   hat_noise(cut=10000, adec=0.035, reso=0.2, drive=0.25),
  "DMX Hat Op":   hat_noise(cut=10000, adec=0.38, reso=0.2, drive=0.25),
  "DMX Tom":      tom(cut=1300, sweep=9, adec=0.26, drive=0.3, vel=0.5),
  "DMX Crash":    cymbal(cut=7000, adec=3.0, drive=0.2),
  "DMX Cowbell":  cowbell(f1oct=1, det=31, cut=1600, adec=0.22, drive=0.2),

  # ---- RX5: mid-80s digital sheen; sharp attacks, bright metallics, aggressive toms + rimshot ----
  "RX5 Kick":     kick(cut=2200, sweep=22, fdec=0.015, adec=0.2, oct=-1, noise=0.15, vel=0.4),
  "RX5 Snare":    snare(tone=0.5, noise=0.85, cut=3000, adec=0.13, reso=0.3, drive=0.15, o2=+7, sweep=8),
  "RX5 Rim":      rim(cut=3200, adec=0.04, noise=0.35, drive=0.2),                            # aggressive
  "RX5 Clap":     clap(cut=1800, adec=0.11, drive=0.1),
  "RX5 Hat Cl":   hat_noise(cut=12500, adec=0.03, reso=0.25),                                 # bright metallic
  "RX5 Hat Op":   hat_noise(cut=12500, adec=0.34, reso=0.25),
  "RX5 Tom":      tom(cut=1600, sweep=10, adec=0.24, drive=0.15, vel=0.55),                    # aggressive
  "RX5 Ride":     cymbal(cut=8500, adec=0.9, noise=0.55),
  "RX5 Crash":    cymbal(cut=8000, adec=3.0),

  # ---- R50: crisp late-80s PCM; clean, punchy, slightly clinical top ----
  "R50 Kick":     kick(cut=1500, sweep=19, fdec=0.018, adec=0.2, oct=-1, noise=0.08, vel=0.4),
  "R50 Snare":    snare(tone=0.45, noise=0.82, cut=2700, adec=0.12, reso=0.25, sweep=7),       # clean punch
  "R50 Rim":      rim(cut=2900, adec=0.04),
  "R50 Clap":     clap(cut=1600, adec=0.11),
  "R50 Hat Cl":   hat_noise(cut=11800, adec=0.03, reso=0.22),                                  # clinical top
  "R50 Hat Op":   hat_noise(cut=11800, adec=0.32, reso=0.22),
  "R50 Tom":      tom(cut=1400, sweep=9, adec=0.22, vel=0.5),
  "R50 Ride":     cymbal(cut=8200, adec=0.85, noise=0.5),
  "R50 Crash":    cymbal(cut=7500, adec=2.9),

  # ---- MP60: boom-bap; deep rounded kick, dusty snare, dark hats; SAT-forward warmth is the point ----
  "MP60 Kick":    kick(cut=650, sweep=15, fdec=0.045, adec=0.34, oct=-1, drive=0.3, vel=0.5),  # deep, rounded, driven
  "MP60 Snare":   snare(tone=0.4, noise=0.65, cut=1600, adec=0.15, reso=0.2, drive=0.35, sweep=5),  # dusty
  "MP60 Rim":     rim(cut=2000, adec=0.05, drive=0.2),
  "MP60 Clap":    clap(cut=1200, adec=0.12, drive=0.2),
  "MP60 Hat Cl":  hat_noise(cut=7000, adec=0.04, reso=0.15, drive=0.15),                        # dark
  "MP60 Hat Op":  hat_noise(cut=7000, adec=0.34, reso=0.15, drive=0.15),
  "MP60 Tom":     tom(cut=900, sweep=8, adec=0.3, drive=0.25, vel=0.6),
  "MP60 Conga":   perc_sine(oct=1, cut=1100, adec=0.2, sweep=5, vel=0.5),
  "MP60 Ride":    cymbal(cut=6500, adec=0.85, drive=0.15, noise=0.45),                          # dark ride
}

# ============================ Shared builders (Phase C C0) ============================
# New voicing languages the three 1.0 kits (and the harmonization pass) are built from.
# All of them are PYTHON ONLY -- they emit parameter values the engine already supports;
# nothing here needs an engine change.

def _params_header():
    return open(os.path.join(os.path.dirname(__file__), "..", "Source", "Parameters.h")).read()

def assert_params_exist(*ids):
    """Fail loudly if a parameter this generator emits is not registered.

    Without this a renamed id would produce presets full of keys the plugin silently
    ignores -- the pad would load, sound wrong, and nothing would say why."""
    src = _params_header()
    missing = [i for i in ids if f'"{i}"' not in src]
    if missing:
        raise SystemExit(f"gen_drumkits: parameters not in Source/Parameters.h: {missing}")

# ---- the NOISE XY field (Phase B) --------------------------------------------------
# The field's defaults (0.5, 0.0) ARE its bypass, so only emit these keys on pads that
# actually shape their noise; an untouched pad stays byte-for-byte as it was.
NOISE_MIN_HZ, NOISE_MAX_HZ = 40.0, 12000.0

def tilt_noise(level, x):
    """Noise at a spectral tilt. x: 0 brown, 0.25 pink, 0.5 white, 1 bright."""
    return dict(noise_level=round(level, 4), noise_x=round(min(max(x, 0.0), 1.0), 4), noise_y=0.0)

def focus_noise(level, hz, focus):
    """Noise squeezed into a band centred on `hz` (40 Hz..12 kHz). focus 0..1 = how
       tight (1 rings into pitched noise). The Hz->axis map mirrors NoiseShaper::focusHz."""
    x = math.log(min(max(hz, NOISE_MIN_HZ), NOISE_MAX_HZ) / NOISE_MIN_HZ) / math.log(NOISE_MAX_HZ / NOISE_MIN_HZ)
    return dict(noise_level=round(level, 4), noise_x=round(x, 4), noise_y=round(min(max(focus, 0.0), 1.0), 4))

# ---- FM metal -----------------------------------------------------------------------
def fm_clang(oct=1, ratio2=7, ratio3=13, fm1=0.8, fm2=0.5, cut=4000, adec=0.3,
             reso=0.2, drive=0.0, vel=0.5, det=0.0, noise=None):
    """Inharmonic metal from the osc3 -> osc2 -> osc1 phase-mod chain.

    Sine carriers (the only waves the engine will phase-modulate -- saw/square rely on
    PolyBLEP edge corrections that a phase offset invalidates). The modulators sit at
    LEVEL 0: they are heard only through the carrier's phase, which is what makes the
    result read as one struck metal object rather than three stacked tones. Non-integer
    SEMI ratios are what make it clang instead of chime."""
    p = d(osc1_wave=3, osc1_octave=oct, osc1_level=0.9,
          osc2_on=1, osc2_wave=3, osc2_octave=oct, osc2_semi=ratio2, osc2_level=0.0, osc2_detune=det,
          osc3_on=1, osc3_wave=3, osc3_octave=oct, osc3_semi=ratio3, osc3_level=0.0,
          osc1_fm=fm1, osc2_fm=fm2,
          filter_type=1, filter_cutoff=cut, filter_reso=reso,
          flt_decay=min(0.05, adec), amp_decay=adec, amp_release=adec * 0.4,
          filter_drive=drive, vel_to_amp=vel)
    if noise: p.update(noise)
    return p

# ---- cymbals from explicit partials --------------------------------------------------
def chord_cymbal(notes, cut=7000, adec=2.5, reso=0.12, drive=0.0, vel=0.5,
                 levels=(0.22, 0.2, 0.18), noise=None, waves=(1, 1, 1)):
    """A cymbal voiced as 2-3 DELIBERATELY chosen inharmonic partials plus a noise bed.

    `notes` are semitone offsets for osc1/2/3 -- picking them by ear (rather than the
    fixed detune spread the classic-machine cymbal() uses) is what lets a ride and a
    crash from the same kit share a family resemblance while ringing differently."""
    n = list(notes) + [0] * (3 - len(notes))
    p = d(osc1_wave=waves[0], osc1_octave=2, osc1_semi=n[0], osc1_level=levels[0],
          osc2_on=1, osc2_wave=waves[1], osc2_octave=2, osc2_semi=n[1], osc2_level=levels[1],
          osc3_on=1, osc3_wave=waves[2], osc3_octave=2, osc3_semi=n[2], osc3_level=levels[2],
          filter_type=1, filter_cutoff=cut, filter_reso=reso,
          flt_decay=0.1, flt_sustain=1.0, flt_release=0.1,
          amp_decay=adec, amp_release=adec * 0.4, filter_drive=drive, vel_to_amp=vel)
    p.update(noise if noise else tilt_noise(0.55, 0.8))
    return p

# ---- pitched drum body ----------------------------------------------------------------
def membrane_tom(oct=0, semi=0, sweep=8, adec=0.3, cut=1200, skin=0.0, skin_ratio=19,
                 drive=0.0, vel=0.55, wave=2, noise=None):
    """A drum with a real PITCH: a sine/triangle body dropped fast by the mod envelope,
       with an optional inharmonic 'skin' partial via FM. The sounding note carries the
       musical pitch, so a kit can hand its toms out as playable intervals."""
    p = d(osc1_wave=wave, osc1_octave=oct, osc1_semi=semi, osc1_level=0.9,
          fltenv_to_pitch=sweep, flt_decay=0.07, amp_decay=adec,
          filter_cutoff=cut, filter_drive=drive, vel_to_amp=vel)
    if skin > 0.0:
        p.update(osc2_on=1, osc2_wave=3, osc2_octave=oct, osc2_semi=skin_ratio,
                 osc2_level=0.0, osc1_fm=skin)
    if noise: p.update(noise)
    return p

# ============ Increment 3: kit-library harmonization (Phase C, C0.5) ============
# Every kit gets the foundational eight, on the notes the sequencer's default rows use.
# The pads below are the ones the library was MISSING; they are voiced in each kit's own
# ingredient language rather than a generic ride/crash dropped into twelve kits.
PRESETS3 = {
  # 808 -- its "Cymbal" becomes the crash; the ride is the same oscillator-bank recipe
  # held tighter and shorter, so the two read as one instrument struck two ways.
  "808 Ride":     chord_cymbal((0, 6, 11), cut=7200, adec=0.95, noise=tilt_noise(0.45, 0.82)),

  # 606 -- thin, sharp, clicky. A metallic PING rather than a wash, and the rim the kit
  # never had (the 606 has no rimshot; this is the kit's click language on a short body).
  "606 Ride":     chord_cymbal((0, 7, 13), cut=9000, adec=0.7, levels=(0.2, 0.16, 0.14),
                               noise=tilt_noise(0.4, 0.9)),
  "606 Rim":      rim(cut=3400, adec=0.035, noise=0.3),

  # CR-78 -- dark and brushy. The ride is a TICK with a tail, not a wash; the crash is a
  # soft warm swell; the rim is woody rather than metallic, matching the kit's bossa blocks.
  "78 Ride":      chord_cymbal((0, 5, 10), cut=5200, adec=0.8, levels=(0.18, 0.15, 0.13),
                               noise=tilt_noise(0.5, 0.55)),
  "78 Crash":     chord_cymbal((0, 6, 11), cut=5600, adec=2.2, levels=(0.18, 0.16, 0.14),
                               noise=tilt_noise(0.6, 0.6)),
  "78 Rim":       d(osc1_wave=2, osc1_octave=1, osc1_level=0.55, filter_type=2,
                    filter_cutoff=1900, filter_reso=0.25, fltenv_to_pitch=6, flt_decay=0.02,
                    amp_decay=0.045, vel_to_amp=0.5, **tilt_noise(0.2, 0.3)),

  # LM1 -- gated real drums, warm-dark top, no cymbals in the original. John's brief: the
  # ride Linn couldn't afford -- deliberately BAND-LIMITED, dull and short, so it sounds
  # like a sample that ran out of bits rather than a modern cymbal.
  "LM1 Ride":     chord_cymbal((0, 6, 11), cut=5000, adec=0.75, levels=(0.2, 0.17, 0.15),
                               drive=0.12, noise=focus_noise(0.45, 6000, 0.22)),
  "LM1 Crash":    chord_cymbal((0, 5, 10), cut=5200, adec=2.0, levels=(0.2, 0.18, 0.16),
                               drive=0.12, noise=focus_noise(0.55, 5200, 0.2)),

  # DMX -- brighter, crunchier early-sample character; SAT-forward like the rest of the kit.
  "DMX Ride":     chord_cymbal((0, 7, 14), cut=8200, adec=0.85, drive=0.22,
                               noise=tilt_noise(0.5, 0.85)),

  # MP60 -- boom-bap. It ships a dark ride and no crash; this is a crash in kind: dusty,
  # rolled off, driven, so it sits under a loop instead of on top of it.
  "MP60 Crash":   chord_cymbal((0, 6, 11), cut=6200, adec=2.6, drive=0.2,
                               noise=tilt_noise(0.6, 0.5)),
}

def slugify(n): return re.sub(r'[^a-z0-9]+', '_', n.lower()).strip('_')

def existing_names():
    names = {}
    for f in os.listdir(OUT):
        if f.endswith(".json"):
            try:
                nm = json.load(open(os.path.join(OUT, f))).get("name")
                if nm: names[nm] = f
            except Exception: pass
    return names

def main():
    # Anything the builders emit must be a real parameter, or the pads would carry keys
    # the plugin silently drops. Checked before a single file is written.
    assert_params_exist("noise_level", "noise_x", "noise_y", "osc1_fm", "osc2_fm",
                        "osc1_semi", "osc2_semi", "osc3_semi", "fltenv_to_pitch", "filter_drive")
    have = existing_names()
    nums = [int(m.group(1)) for f in os.listdir(OUT) if (m:=re.match(r'(\d+)_', f))]
    nxt = max(nums) + 1
    wrote = 0
    allp = dict(PRESETS); allp.update(PRESETS2); allp.update(PRESETS3)   # increment 1 + 2 + 3
    for name, params in allp.items():
        obj = {"name": name, "category": "Drums", "params": params}
        if name in have:
            fn = have[name]                       # overwrite in place (keeps the number stable)
        else:
            fn = f"{nxt}_{slugify(name)}.json"; nxt += 1
        json.dump(obj, open(os.path.join(OUT, fn), "w"), indent=2)
        open(os.path.join(OUT, fn), "a").write("\n")
        wrote += 1
    print(f"wrote {wrote} drum presets (next free number {nxt})")

if __name__ == "__main__":
    main()
