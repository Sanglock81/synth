# Releasing synth (tag-time runbook for v1.0.0)

The tag is a **deliberate, human-triggered** cut. It fires **only on the maintainer's explicit
call**, after the performance validation and the UAT are done and every **BLOCKER**
defect is fixed + re-gated. This runbook makes the actual cut a clean, one-pass job.

## Pre-tag PERFORMANCE GATE (blocking, applies to `v1.0.0-rc1` and every later cut)

**This gate does not require a quiesced machine.** An earlier draft demanded one; that was
written on a false premise — the #100 baseline in `docs/target-report.txt` was itself taken
under *normal* machine conditions, with other services running. What that report pins is the
**governor**, not an idle desktop.

Maintainer's ruling, quoted:

> "No, I'm not doing that now and never have - there are other services running on this
> laptop, we'll have to schedule that if it's really necessary, and I don't think it is."

So the gate is satisfied **arithmetically plus in the real world**, not by chasing a clean
absolute bench run:

**(a) Differential against the #100 baseline.** A change's cost is measured as a *delta*
between rows in the same run — a "feature bypassed" row beside the engaged ones — and added
to the corresponding `docs/target-report.txt` figure. Absolute percentages from an ad-hoc run
are **not** the gate; they move by 20–50% with ordinary background load, which is why
unchanged code can read over 30% on a busy afternoon. NOISE XY worked out as:

| Row (#100 baseline) | Baseline | + field (worst, +2.3 pts) |
|---|---|---|
| Realistic live set (4 parts) | 21.2% | **≈23%** |
| Per-part mod matrix + block-mods | 20.0% | ≈22% |
| 4 parts × 4v + 4× ALL FX | 19.9% | ≈22% |
| 24v + ALL FX (full pool) | 26.7% | ≈29% |

- [ ] Governor is `performance` on every core
      (`cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor`). Still mandatory — at
      `powersave` the bench roughly doubles and fails for no real reason.
- [ ] `./build/tests/dsp_bench` run **alone**, not alongside `ctest` (a concurrent test suite
      has read 278% of budget on unchanged code — that number means nothing).
- [ ] The feature's **delta** measured against a bypassed row in the same run, added to the
      matching `target-report.txt` row, keeps every **live** configuration under 30%.

**(b) The absolute real-world validation is UAT section C** — the uninterrupted integration
set, played on this machine under normal conditions. **Zero overruns required.** That, not a
synthetic bench, is what proves the thing gigs.

**(c) The `[synthetic]` bench row** (`24v noise+field+LFO+driven`: full pool, noise 0.9 on
every voice, LFO on both field axes at full depth, driven/self-oscillating filter) is a
**stress row in the `runs` class**. It must complete without an overrun; it carries **no 30%
requirement**, because nothing plays like that.

- [ ] `docs/target-report.txt` is **not** regenerated to paper over a change. It is a
      baseline, and it is replaced only by a deliberate, dated re-measurement.

## Pre-flight — all must be true
- [ ] `git status` clean, on `master`, up to date with origin.
- [ ] **Performance validation** complete and acceptable (latency + no xruns + voice cap settled).
- [ ] **UAT** signed off; all BLOCKER defects fixed, re-gated, and CI green on **both** platforms.
- [ ] Known-issues (KNOWN-ISSUE from UAT + the pluginval teardown flake) captured in the CHANGELOG.
- [ ] `./run-all-checks.sh` **and** `./run-all-checks.sh --sanitize` green locally on the commit you'll tag.
- [ ] Register copyright with the US Copyright Office (eCO → Literary Work, ~$45; deposit PDF in docs/) before wide sharing.

## Step 1 — confirm the version
The version string is `1.0.0` in `CMakeLists.txt` (`project(VASynth VERSION 1.0.0)`); the on-screen
banner shows that plus a build-time git short hash. **No code change needed** to tag 1.0.0 — just
confirm it reads `1.0.0`. (For any later release, bump this line first.)

## Step 2 — reconcile the CHANGELOG  ⚠️ decision required
The CHANGELOG has a `## [1.0.0] — 2026-07-13` section **and** an `## [Unreleased]` section whose own
header calls it "post-1.0 work … not yet tagged." Because **v1.0.0 was never actually git-tagged**,
everything under `[Unreleased]` (wavetable, unison, QWERTY, session export, factory content, TRIM,
menu grouping, …) is part of the release you're about to cut.

**Recommended reconciliation:** fold `[Unreleased]` **into** the `[1.0.0]` section and set the
`[1.0.0]` date to the **tag date**, so the single `[1.0.0]` entry describes exactly what ships.
Leave a fresh empty `[Unreleased]` at the top for post-tag work.

```
## [Unreleased]

## [1.0.0] — <TAG DATE>
<everything that was under [Unreleased], merged above the old 2026-07-13 body>
```

(Alternative, if you decide the extra work is a separate line: label it `[1.0.1]`/`[1.1.0]` instead
and tag that. The recommended path keeps one honest 1.0.0.)

Draft release notes are already prepared in **[release-notes-1.0.0.md](release-notes-1.0.0.md)** —
edit to match the reconciliation above, then reuse as the GitHub release body.

## Step 3 — final gate on the exact commit
```bash
./run-all-checks.sh && ./run-all-checks.sh --sanitize
```
Commit the CHANGELOG reconciliation. This is the commit you tag.

## Step 4 — build the packages
```bash
./scripts/package-release.sh            # -> dist/synth-1.0.0-linux-x86_64.tar.gz (+ .sha256)
```
The tarball has the prebuilt standalone + VST3 + docs + a copy-only `install.sh`.

**Windows package** (on the Windows machine, after `install-windows.ps1` has built Release):
```powershell
$v="1.0.0"
$art="build\VASynth_artefacts\Release"
$dst="dist\synth-$v-windows-x64"; New-Item -ItemType Directory -Force $dst,"$dst\bin","$dst\vst3" | Out-Null
Copy-Item "$art\Standalone\synth.exe" "$dst\bin\"
Copy-Item -Recurse "$art\VST3\synth.vst3" "$dst\vst3\"
Copy-Item README.md,LICENSE,CHANGELOG.md,docs\INSTALL.md $dst
Compress-Archive "$dst\*" "dist\synth-$v-windows-x64.zip" -Force
(Get-FileHash "dist\synth-$v-windows-x64.zip" -Algorithm SHA256).Hash > "dist\synth-$v-windows-x64.zip.sha256"
```

## Step 5 — tag
```bash
git tag -a v1.0.0 -m "synth 1.0.0"
git push origin v1.0.0
```
Tagging is the point of no return — do it only after Steps 1–4 pass.

## Step 6 — GitHub release
```bash
gh release create v1.0.0 \
  dist/synth-1.0.0-linux-x86_64.tar.gz dist/synth-1.0.0-linux-x86_64.tar.gz.sha256 \
  dist/synth-1.0.0-windows-x64.zip     dist/synth-1.0.0-windows-x64.zip.sha256 \
  --title "synth 1.0.0" --notes-file docs/release-notes-1.0.0.md
```
(Attach the Windows artifacts once built; drop them from the command if Linux-only.)

## Step 7 — after the tag
- [ ] Verify the banner in a fresh build shows a hash at/after the tag (`git describe` now finds `v1.0.0`).
- [ ] The freeze lifts — 1.1 opens with **Sessions first**, then the Live Rig.
- [ ] Announce / update the README roadmap (`v1.0.0 — shipped` becomes literally true).

---

### Notes
- `dist/` is git-ignored — release artifacts are never committed; they live only on the GitHub release.
- Nothing here runs automatically. `package-release.sh` only packages; it never tags, pushes, or edits
  the CHANGELOG.
