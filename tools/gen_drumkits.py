#!/usr/bin/env python3
"""Generator for the synthesized classic-machine drum presets (kit building blocks).

Kits render DRY (no FX block), so grit comes from filter_drive (per-voice), never fx_sat/reverb.
Character over accuracy: documented tunings are the starting point; voiced to sound good.
Emits resources/presets/<NNN>_<slug>.json. Run from repo root: python3 tools/gen_drumkits.py
Idempotent by preset NAME; re-run after editing the spec. Increment 1 = 808 / 909 / 606 / 78.
"""
import json, os, re

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
    have = existing_names()
    nums = [int(m.group(1)) for f in os.listdir(OUT) if (m:=re.match(r'(\d+)_', f))]
    nxt = max(nums) + 1
    wrote = 0
    allp = dict(PRESETS); allp.update(PRESETS2)     # increment 1 + 2
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
