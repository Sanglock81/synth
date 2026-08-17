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

# ==================== FOUNDRY (Phase C, C1) — industrial / rock ====================
# A struck-metal workshop: driven mechanical kicks, metal that CLANGS rather than chimes
# (the FM chain's inharmonic ratios), oil-drum toms, and machine noises for colour.
# Renders DRY like every kit here -- all the grit is filter_drive inside the voice, never
# an FX block, so a Foundry pad sounds the same wherever it is dropped.
FOUNDRY = {
  # --- kicks: mechanical, not round. A fast pitch drop plus hard in-loop drive is the
  # difference between a kick and a machine STAMPING.
  "Foundry Stamp":    kick(cut=900, sweep=26, fdec=0.018, adec=0.20, oct=-1, drive=0.75,
                           noise=0.22, vel=0.45),
  "Foundry Boom":     kick(cut=520, sweep=18, fdec=0.05, adec=0.52, oct=-1, drive=0.55,
                           vel=0.5),

  # --- snare: a tone stab under a noise burst, with the noise FOCUSED into a crack band
  # rather than left as broadband hiss -- that band is what reads as "hit metal sheet".
  "Foundry Blast":    dict(snare(tone=0.5, noise=0.85, cut=2300, adec=0.18, reso=0.35,
                                 drive=0.6, o2=+7, sweep=8, vel=0.55),
                           **focus_noise(0.85, 2400, 0.42)),
  "Foundry Tick":     fm_clang(oct=2, ratio2=11, ratio3=17, fm1=0.55, fm2=0.35, cut=3600,
                               adec=0.05, drive=0.3, vel=0.5),

  # --- the metal pair. Anvil rings BRIGHT and medium; Pipe is the same instrument struck
  # low and hollow. They choke each other, so a fast alternation reads as one object.
  "Foundry Anvil":    fm_clang(oct=2, ratio2=6, ratio3=13, fm1=0.85, fm2=0.6, cut=6000,
                               adec=0.55, drive=0.35, vel=0.55,
                               noise=tilt_noise(0.18, 0.8)),
  "Foundry Pipe":     fm_clang(oct=0, ratio2=8, ratio3=15, fm1=0.9, fm2=0.7, cut=1800,
                               adec=0.75, reso=0.3, drive=0.4, vel=0.55,
                               noise=focus_noise(0.22, 320, 0.5)),

  # --- oil-drum toms: a real pitched body with an inharmonic SKIN partial on top, driven.
  # The sounding note carries the pitch, so these stay playable as intervals.
  "Foundry Drum Lo":  membrane_tom(oct=-1, sweep=10, adec=0.42, cut=900, skin=0.45,
                                   skin_ratio=17, drive=0.45, vel=0.6,
                                   noise=tilt_noise(0.15, 0.15)),
  "Foundry Drum Mid": membrane_tom(oct=0, semi=-5, sweep=9, adec=0.36, cut=1150, skin=0.48,
                                   skin_ratio=18, drive=0.45, vel=0.6,
                                   noise=tilt_noise(0.15, 0.18)),
  "Foundry Drum Hi":  membrane_tom(oct=0, sweep=9, adec=0.3, cut=1400, skin=0.5,
                                   skin_ratio=19, drive=0.45, vel=0.6,
                                   noise=tilt_noise(0.15, 0.2)),

  # --- hats: a metal SHARD rather than a hat -- short FM metal with a focused noise edge;
  # the open one is driven noise so the pair still behaves like hats under a groove.
  "Foundry Shard":    fm_clang(oct=2, ratio2=10, ratio3=18, fm1=0.7, fm2=0.5, cut=9000,
                               adec=0.05, drive=0.3, vel=0.45,
                               noise=focus_noise(0.35, 8000, 0.3)),
  "Foundry Hiss":     d(osc1_on=0, osc1_level=0, filter_type=1, filter_cutoff=8000,
                        filter_reso=0.2, flt_decay=0.04, amp_decay=0.4, filter_drive=0.45,
                        vel_to_amp=0.45, **tilt_noise(0.9, 0.78)),

  # --- cymbals, from deliberately chosen partials so crash and ride are audibly the same
  # metal struck differently: the crash spreads wide and washes, the ride keeps a sizzle
  # bed focused around its ping.
  "Foundry Crash":    chord_cymbal((0, 6, 13), cut=7400, adec=3.2, drive=0.3,
                                   levels=(0.24, 0.22, 0.2), noise=tilt_noise(0.7, 0.85)),
  "Foundry Ride Sizzle": chord_cymbal((0, 7, 16), cut=8600, adec=1.3, drive=0.25,
                                   levels=(0.22, 0.18, 0.15), noise=focus_noise(0.5, 7000, 0.35)),

  # --- machine colour. Chain is a gated rattle: a very short amp with a hard-focused
  # noise band, so repeated hits read as links rather than a wash. (One voice cannot fire
  # several transients from one trigger, so the rattle lives in the band, not in a
  # multi-hit envelope -- see the kit note in docs/presets.md.)
  "Foundry Chain":    d(osc1_on=0, osc1_level=0, filter_type=1, filter_cutoff=6000,
                        filter_reso=0.4, flt_decay=0.02, amp_attack=0.001, amp_decay=0.14,
                        amp_release=0.05, filter_drive=0.5, vel_to_amp=0.55,
                        **focus_noise(0.95, 5200, 0.62)),
  # Hydraulic press: a low thud with a descending focused whoosh riding on top of it.
  "Foundry Press":    d(osc1_wave=3, osc1_octave=-1, osc1_level=0.7, fltenv_to_pitch=-14,
                        filter_type=0, filter_cutoff=700, filter_reso=0.3,
                        flt_attack=0.002, flt_decay=0.5, flt_sustain=0.0,
                        amp_decay=0.55, amp_release=0.2, filter_drive=0.5, vel_to_amp=0.5,
                        **focus_noise(0.7, 900, 0.55)),
  # A very short mid machine tick -- the workshop's metronome.
  "Foundry Cycle":    fm_clang(oct=1, ratio2=9, ratio3=14, fm1=0.6, fm2=0.45, cut=2600,
                               adec=0.07, drive=0.35, vel=0.5),
  # Powerdown: the FM chain falling off a cliff, with the noise band sinking after it.
  "Foundry Powerdown": d(osc1_wave=3, osc1_octave=1, osc1_level=0.8,
                         osc2_on=1, osc2_wave=3, osc2_octave=1, osc2_semi=13, osc2_level=0.0,
                         osc3_on=1, osc3_wave=3, osc3_octave=1, osc3_semi=7, osc3_level=0.0,
                         osc1_fm=0.8, osc2_fm=0.5, fltenv_to_pitch=-30,
                         filter_type=0, filter_cutoff=3000, filter_reso=0.35,
                         flt_attack=0.002, flt_decay=0.8, flt_sustain=0.0,
                         amp_decay=0.9, amp_release=0.3, filter_drive=0.4, vel_to_amp=0.5,
                         **focus_noise(0.5, 2000, 0.5)),
}

# Wavetable choice indices (Parameters.h wtKindNames). Kits resolve the FACTORY table, so a
# kit pad may use WT without owning a random seed.
WT_ANALOG, WT_SWEEP, WT_VOWEL, WT_DIGITAL = 0, 1, 2, 3
WT_WAVE = 4   # PolyBlepOscillator::Wave::Wavetable

# ==================== CIRCUIT (Phase C, C2) — modern electronic ====================
# The brief this kit is answering: do NOT sound like another classic-machine homage. So
# nothing here is a 606/808/909 voicing with the knobs moved. Its language is instead the
# newest things the engine can do -- wavetables for the tone sources and the Phase B noise
# FIELD for everything hissy, which is what a filtered-noise hat cannot give you: a hat
# whose colour is a FOCUSED BAND rather than a high-passed wash reads as sample-clean.
CIRCUIT = {
  # --- kicks: tight and modern. A short, hard sine sweep with almost no tail, and a
  # sub-drop sibling that trades the click for weight.
  "Circuit Kick":     kick(cut=1600, sweep=30, fdec=0.012, adec=0.24, oct=-1, drive=0.18,
                           noise=0.06, vel=0.35),
  "Circuit Sub":      kick(cut=420, sweep=16, fdec=0.03, adec=0.62, oct=-2, drive=0.1, vel=0.4),

  # --- snares + clap. The clap is WIDE (a long-ish focused burst rather than a tight tick);
  # the roll is the same snare cut to a stutter length for fills.
  "Circuit Snare":    dict(snare(tone=0.5, noise=0.9, cut=2900, adec=0.12, reso=0.3,
                                 drive=0.12, o2=+7, sweep=8, vel=0.55),
                           **focus_noise(0.9, 3000, 0.3)),
  "Circuit Roll":     dict(snare(tone=0.4, noise=0.85, cut=3200, adec=0.05, reso=0.35,
                                 sweep=9, vel=0.6),
                           **focus_noise(0.85, 3400, 0.35)),
  "Circuit Clap":     d(osc1_on=0, osc1_level=0, filter_type=2, filter_cutoff=1700,
                        filter_reso=0.35, flt_decay=0.11, amp_attack=0.004, amp_decay=0.17,
                        amp_release=0.08, vel_to_amp=0.5, **focus_noise(0.95, 1800, 0.28)),
  # A clean electronic click rather than a wooden rim -- the kit had no rim; this is the
  # convention's pad in Circuit's own language.
  "Circuit Rim":      d(osc1_wave=3, osc1_octave=2, osc1_level=0.5, fltenv_to_pitch=10,
                        filter_type=1, filter_cutoff=3200, filter_reso=0.2, flt_decay=0.012,
                        amp_decay=0.035, vel_to_amp=0.5, **focus_noise(0.35, 4200, 0.4)),

  # --- hats: focused-noise, not high-passed hiss. Bright, tight, and audibly NOT an 808.
  "Circuit Hat Cl":   d(osc1_on=0, osc1_level=0, filter_type=1, filter_cutoff=11000,
                        filter_reso=0.15, flt_decay=0.02, amp_decay=0.03, vel_to_amp=0.45,
                        **focus_noise(0.9, 9500, 0.45)),
  "Circuit Hat Op":   d(osc1_on=0, osc1_level=0, filter_type=1, filter_cutoff=11000,
                        filter_reso=0.15, flt_decay=0.04, amp_decay=0.32, vel_to_amp=0.45,
                        **focus_noise(0.9, 9000, 0.38)),

  # --- pitched wavetable perc. This is Circuit's tom family: a real pitch, so the three
  # tom rows are playable as intervals, but a digital blip rather than a drum skin.
  "Circuit Blip":     d(osc1_wave=WT_WAVE, osc1_wt_kind=WT_DIGITAL, osc1_wt_pos=0.35,
                        osc1_octave=0, osc1_level=0.85, fltenv_to_pitch=12,
                        filter_type=0, filter_cutoff=3800, filter_reso=0.25,
                        flt_decay=0.05, amp_decay=0.22, amp_release=0.06, vel_to_amp=0.6),

  # --- cymbals: wavetable partials over a bright tilted bed, so they read as synthetic
  # rather than as a sampled cymbal.
  "Circuit Ride":     chord_cymbal((0, 7, 14), cut=9500, adec=1.1, levels=(0.2, 0.16, 0.13),
                                   waves=(WT_WAVE, 1, 1), noise=tilt_noise(0.5, 0.9)),
  "Circuit Crash":    chord_cymbal((0, 6, 13), cut=8800, adec=2.9, levels=(0.22, 0.2, 0.18),
                                   waves=(WT_WAVE, 1, 1), noise=tilt_noise(0.7, 0.92)),

  # --- colour. Shaker: a high, tight focused band. Zap: the field's focus swept hard and
  # fast at near-ring Q -- a sound this engine simply could not make before Phase B.
  "Circuit Shaker":   d(osc1_on=0, osc1_level=0, filter_type=1, filter_cutoff=9000,
                        filter_reso=0.2, flt_decay=0.02, amp_attack=0.002, amp_decay=0.05,
                        vel_to_amp=0.5, **focus_noise(0.85, 8500, 0.5)),
  "Circuit Zap":      d(osc1_on=0, osc1_level=0, filter_type=0, filter_cutoff=9000,
                        filter_reso=0.1, filter_env_amt=-0.9, flt_attack=0.001, flt_decay=0.09,
                        flt_sustain=0.0, amp_decay=0.11, amp_release=0.04, vel_to_amp=0.5,
                        **focus_noise(0.95, 5000, 0.92)),
  # Riser: a slow swell whose focused band climbs -- the filter envelope opens upward while
  # the amp fades in, so it lifts into a downbeat instead of just getting louder.
  "Circuit Riser":    d(osc1_on=0, osc1_level=0, filter_type=1, filter_cutoff=800,
                        filter_reso=0.35, filter_env_amt=0.95,
                        flt_attack=0.9, flt_decay=0.3, flt_sustain=1.0, flt_release=0.2,
                        amp_attack=0.85, amp_decay=0.5, amp_sustain=0.0, amp_release=0.12,
                        vel_to_amp=0.4, **focus_noise(0.9, 1200, 0.6)),
}

# ==================== HEARTH (Phase C, C3) — warm, folk-adjacent ====================
# The hardest of the three: everything here has to sound STRUCK and WOODEN rather than
# switched on. Two ideas carry it. First, drive stays at (or near) zero throughout -- the
# warmth comes from where the energy sits, not from saturation. Second, the noise the kit
# is full of is PINK-TILTED rather than white: brushes, shakers and skins have their energy
# down low, and a white-noise bed is exactly what makes a synthetic kit sound synthetic.
# The toms are real pitches so they can be played as a melody, which is a stated goal.
HEARTH = {
  # --- felt kick: round, long-ish, no click and no drive at all.
  "Hearth Kick":      dict(kick(cut=380, sweep=12, fdec=0.06, adec=0.42, oct=-1, drive=0.0,
                                vel=0.55),
                           **tilt_noise(0.08, 0.12)),
  # Cajon slap: a body thump with a focused mid CRACK on top -- the two halves of hitting
  # a wooden box, one at the centre and one at the corner.
  "Hearth Cajon":     d(osc1_wave=2, osc1_octave=-1, osc1_level=0.7, fltenv_to_pitch=9,
                        filter_type=0, filter_cutoff=700, filter_reso=0.2, flt_decay=0.03,
                        amp_decay=0.22, amp_release=0.07, vel_to_amp=0.65,
                        **focus_noise(0.55, 1500, 0.4)),

  # --- brushes. The bed is pink so it reads as wire on a head rather than hiss; the swell
  # is the same brush taken with a slow attack, for the pull across the drum.
  "Hearth Brush":     dict(snare(tone=0.28, noise=0.7, cut=1400, adec=0.15, reso=0.18,
                                 sweep=4, vel=0.6),
                           **tilt_noise(0.7, 0.25)),
  "Hearth Swell":     d(osc1_wave=3, osc1_octave=0, osc1_level=0.18, filter_type=2,
                        filter_cutoff=1500, filter_reso=0.15, flt_decay=0.2,
                        amp_attack=0.14, amp_decay=0.35, amp_sustain=0.0, amp_release=0.14,
                        vel_to_amp=0.6, **tilt_noise(0.75, 0.22)),
  # Rim click: wood on a rim, short and dry.
  "Hearth Rim":       d(osc1_wave=2, osc1_octave=1, osc1_level=0.6, filter_type=2,
                        filter_cutoff=2100, filter_reso=0.28, fltenv_to_pitch=7,
                        flt_decay=0.015, amp_decay=0.04, vel_to_amp=0.55,
                        **tilt_noise(0.18, 0.35)),
  # Hand clap: looser and softer than Circuit's -- a wider, lower band and a gentler edge.
  "Hearth Clap":      d(osc1_on=0, osc1_level=0, filter_type=2, filter_cutoff=1300,
                        filter_reso=0.3, flt_decay=0.1, amp_attack=0.005, amp_decay=0.15,
                        amp_release=0.08, vel_to_amp=0.55, **focus_noise(0.8, 1400, 0.2)),

  # --- the gentlest hats in the library. Pink-tilted and quiet, so they sit under a
  # fingerpicked part instead of on top of it.
  "Hearth Hat Cl":    d(osc1_on=0, osc1_level=0, filter_type=1, filter_cutoff=6500,
                        filter_reso=0.1, flt_decay=0.018, amp_decay=0.032, vel_to_amp=0.55,
                        **tilt_noise(0.6, 0.45)),
  "Hearth Hat Op":    d(osc1_on=0, osc1_level=0, filter_type=1, filter_cutoff=6200,
                        filter_reso=0.1, flt_decay=0.035, amp_decay=0.26, vel_to_amp=0.55,
                        **tilt_noise(0.6, 0.42)),

  # --- tuned toms: a real pitched body with a soft FM skin partial. Melodic use is the
  # point, so the sweep is gentle and the decay long enough to hear the note.
  "Hearth Tom":       membrane_tom(oct=0, sweep=6, adec=0.45, cut=1100, skin=0.22,
                                   skin_ratio=15, drive=0.0, vel=0.6, wave=2,
                                   noise=tilt_noise(0.12, 0.2)),

  # --- cymbals, consonant-leaning: the partials are a fifth and an octave-ish rather than
  # the deliberately sour intervals Foundry uses, so they wash warmly instead of clanging.
  "Hearth Ride":      chord_cymbal((0, 7, 12), cut=6000, adec=1.6, levels=(0.16, 0.14, 0.12),
                                   noise=tilt_noise(0.5, 0.6)),
  # A real crash for the crash row: light and warm like the rest of the kit, but FULLER and
  # longer than the splash -- more bed under it, a slightly darker top, and enough tail to
  # mark a section rather than just accent a beat.
  "Hearth Crash":     chord_cymbal((0, 7, 12), cut=6200, adec=2.6, levels=(0.21, 0.19, 0.17),
                                   noise=tilt_noise(0.68, 0.58)),
  "Hearth Splash":    chord_cymbal((0, 7, 12), cut=6800, adec=1.1, levels=(0.18, 0.15, 0.13),
                                   noise=tilt_noise(0.55, 0.65)),

  # --- hand percussion.
  "Hearth Tamb":      fm_clang(oct=2, ratio2=12, ratio3=19, fm1=0.5, fm2=0.3, cut=7000,
                               adec=0.16, reso=0.15, drive=0.0, vel=0.55,
                               noise=focus_noise(0.5, 7500, 0.3)),
  "Hearth Shaker":    d(osc1_on=0, osc1_level=0, filter_type=1, filter_cutoff=7000,
                        filter_reso=0.15, flt_decay=0.015, amp_attack=0.003, amp_decay=0.05,
                        vel_to_amp=0.55, **focus_noise(0.75, 6000, 0.35)),
  "Hearth Block":     d(osc1_wave=2, osc1_octave=2, osc1_level=0.75, filter_type=2,
                        filter_cutoff=2600, filter_reso=0.35, fltenv_to_pitch=4,
                        flt_decay=0.02, amp_decay=0.07, vel_to_amp=0.55,
                        **tilt_noise(0.1, 0.3)),
}

def slugify(n): return re.sub(r'[^a-z0-9]+', '_', n.lower()).strip('_')

def existing_names():
    """name -> (filename, category) for every preset already in the bank."""
    names = {}
    for f in os.listdir(OUT):
        if f.endswith(".json"):
            try:
                o = json.load(open(os.path.join(OUT, f)))
                nm = o.get("name")
                if nm: names[nm] = (f, o.get("category"))
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
    allp = dict(PRESETS)
    for extra in (PRESETS2, PRESETS3, FOUNDRY, CIRCUIT, HEARTH):   # inc 1-3 + the three 1.0 kits
        allp.update(extra)
    for name, params in allp.items():
        obj = {"name": name, "category": "Drums", "params": params}
        if name in have:
            fn, cat = have[name]
            # A drum name that collides with a patch from ANOTHER category would overwrite it
            # in place and quietly move it out of its own bank -- which is exactly what the
            # Foundry kick did to a Pluck patch on its first run (that patch is now "Rivet",
            # and the kick is "Foundry Stamp"). Refuse.
            if cat != "Drums":
                raise SystemExit(f"gen_drumkits: '{name}' already exists as a {cat} preset "
                                 f"({fn}) -- pick a different name rather than overwriting it")
            # same category: overwrite in place (keeps the number stable)
        else:
            fn = f"{nxt}_{slugify(name)}.json"; nxt += 1
        json.dump(obj, open(os.path.join(OUT, fn), "w"), indent=2)
        open(os.path.join(OUT, fn), "a").write("\n")
        wrote += 1
    print(f"wrote {wrote} drum presets (next free number {nxt})")

if __name__ == "__main__":
    main()
