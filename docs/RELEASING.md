# Releasing synth (tag-time runbook for v1.0.0)

The tag is a **deliberate, human-triggered** cut. It fires **only on the maintainer's explicit
call**, after the performance validation and the UAT are done and every **BLOCKER**
defect is fixed + re-gated. This runbook makes the actual cut a clean, one-pass job.

## Pre-tag BENCH GATE (blocking, applies to `v1.0.0-rc1` and every later cut)

Added during Phase B, when the bench could not be certified: several rows that shipped under
30% measured *over* it on **unchanged** code, because the machine was not quiesced. A control
run on the pre-feature tree confirmed the inflation was machine state, not the change. The
differential was accepted for that increment; the absolute check moves here.

Maintainer's ruling, quoted:

> "before cutting v1.0.0-rc1, the full bench must be re-run under #100-equivalent quiesced
> conditions (performance governor, desktop/browser closed — John will quiesce or run it
> himself and hand you the numbers if needed). Acceptance at that gate: all live
> configurations ≤30% of budget with the feature engaged at realistic settings. Report the
> synthetic worst-case stack (all voices noise 0.9 + LFO focus max + driven) as its own row,
> verdict deferred to John — he decides whether that stack is a live configuration or a
> torture row, per the report's existing OK-vs-runs distinction. Option 4 is rejected:
> docs/target-report.txt is not re-baselined against an unquiesced machine under any
> circumstances."

- [ ] Governor is `performance` on every core (`cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor`).
      The bench is **invalid** at `powersave` — it roughly doubles and fails for no real reason.
- [ ] Desktop and browser closed; load average settled. Nothing else building or testing.
- [ ] `./build/tests/dsp_bench` run **alone**, not alongside `ctest`.
- [ ] Every **live** configuration ≤ 30% of the 2.667 ms budget with the noise field engaged at
      realistic settings.
- [ ] The synthetic worst-case row (`24v noise+field+LFO+driven`) reported with its number and
      **no verdict** — whether it counts as a live configuration or a torture row is the
      maintainer's call, per this report's existing `OK<30%` vs `runs` distinction.
- [ ] `docs/target-report.txt` is **not** regenerated from an unquiesced machine. Ever.

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
