# Installing synth

**synth** ships in two forms from one build:

- a **Standalone** application (its own window + audio/MIDI device handling), and
- a **VST3 plugin** you load inside a DAW (any VST3 host).

There are no prebuilt binaries yet — you build from source (it's one command once the
tools are in place) and the install scripts place the results where your system and DAW
expect them. JUCE is fetched automatically by CMake; you don't install it yourself.

---

## Quick install

### Linux (Debian / Ubuntu and derivatives)

```bash
git clone https://github.com/Sanglock81/synth.git
cd synth
./scripts/install-linux.sh
```

That installs the apt build dependencies, builds Release, and puts the VST3 in `~/.vst3`
and the standalone in `~/.local/bin/synth` with a menu entry. Options:

```bash
./scripts/install-linux.sh --no-deps          # deps already installed
./scripts/install-linux.sh --vst3-dir DIR     # install the VST3 elsewhere
./scripts/install-linux.sh --prefix ~/.local  # standalone under DIR/bin
./scripts/install-linux.sh --no-desktop       # no menu entry
./scripts/install-linux.sh --help
```

### Windows 10 / 11

Install the prerequisites once (below), then from PowerShell in the repo root:

```powershell
git clone https://github.com/Sanglock81/synth.git
cd synth
.\scripts\install-windows.ps1
```

That builds Release and installs the VST3 to the **per-user** VST3 folder
(`%LOCALAPPDATA%\Programs\Common\VST3`, no admin needed) and the standalone with a Start
Menu shortcut. To install the VST3 system-wide instead, run an **elevated** PowerShell:

```powershell
.\scripts\install-windows.ps1 -Vst3Dir "$env:CommonProgramFiles\VST3"
```

---

## Prerequisites

### Linux
The install script runs this for you (`apt`), but for reference the required system
libraries are:

```
build-essential cmake git pkg-config
libasound2-dev libjack-jackd2-dev
libfreetype-dev libfontconfig1-dev
libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxext-dev
libgl1-mesa-dev
```

(The web-browser and libcurl JUCE modules are disabled in this build, so
`libwebkit2gtk`/`libcurl` are **not** required — the script tries them but never fails on
them.) On Fedora/Arch install the equivalents and run with `--no-deps`.

### Windows
- **Visual Studio 2022** with the **"Desktop development with C++"** workload — or the
  standalone **Build Tools for Visual Studio 2022**.
- **CMake 3.22+** and **Git** on `PATH` (`winget install Kitware.CMake Git.Git`).

---

## Manual build (either OS)

If you'd rather not use the scripts:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release      # Windows: omit the -D, use --config below
cmake --build build --config Release -j 8
```

Artifacts land in `build/VASynth_artefacts/Release/`:

| | Linux | Windows |
|---|---|---|
| Standalone | `Standalone/synth` | `Standalone\synth.exe` |
| VST3 | `VST3/synth.vst3` | `VST3\synth.vst3` |

Copy the VST3 to your plugin folder and run the standalone directly. (On Linux the build
already auto-copies the VST3 to `~/.vst3` unless you configure with
`-DVASYNTH_COPY_PLUGIN=OFF`.)

## Where things get installed

| | Linux (default) | Windows (default) |
|---|---|---|
| Standalone | `~/.local/bin/synth` | `%LOCALAPPDATA%\Programs\synth\synth.exe` |
| VST3 | `~/.vst3/synth.vst3` | `%LOCALAPPDATA%\Programs\Common\VST3\synth.vst3` |
| Launcher | `~/.local/share/applications/synth.desktop` | Start Menu → *synth* |
| **Your data** | `~/.config/synth/` | `%APPDATA%\synth\` |

**Your data** — saved presets, kits, samples, MULTI layouts, learned MIDI maps, and the
log — lives in the data folder above and is **never touched** by install/uninstall/update.

## First run

- **Standalone (Linux):** it opens the PipeWire/default output and makes sound immediately;
  it follows your system default sink, so setting your audio interface as the default
  routes it there. Pick a specific device in **Options → Audio/MIDI Settings** (buffer
  128–256 @ 48 kHz to start). MIDI controllers auto-connect on plug-in.
- **Standalone (Windows):** uses WASAPI by default. For lowest latency into your audio
  interface, load the **VST3 in a DAW on the ASIO driver** rather than the standalone.
- **VST3:** rescan/refresh plugins in your DAW; it appears as **synth** (instrument).

The window shows a **version + git-hash banner** — handy for confirming you're running the
build you think you are.

## Updating

```bash
git pull
./scripts/install-linux.sh            # or .\scripts\install-windows.ps1
```

Re-running the installer reconfigures, rebuilds incrementally, and overwrites the installed
copies. Your data folder is untouched.

## Uninstall

- **Linux:** `./scripts/uninstall-linux.sh` (add `--purge` to also remove `~/.config/synth`).
- **Windows:** delete `%LOCALAPPDATA%\Programs\synth\`, the `synth.vst3` folder in your VST3
  dir, and the Start Menu *synth* shortcut. Your data in `%APPDATA%\synth` stays unless you
  delete it too.

## Verifying the plugin (optional)

The repo's gate runs `pluginval` against the VST3. To validate your installed copy manually,
point [pluginval](https://github.com/Tracktion/pluginval) at `synth.vst3`.

## Troubleshooting

- **DAW doesn't see the plugin** — trigger a plugin **rescan** and confirm your DAW scans the
  install dir above (some DAWs keep their own VST3 path list). On Windows the per-user folder
  `%LOCALAPPDATA%\Programs\Common\VST3` is scanned by most hosts; if yours doesn't, re-run
  the installer with `-Vst3Dir "$env:CommonProgramFiles\VST3"` from an elevated PowerShell.
- **`cmake` not found / too old** — install 3.22+ (Linux: `apt install cmake`; Windows:
  `winget install Kitware.CMake`).
- **Linux build fails on a missing `-dev` lib** — run the installer without `--no-deps`, or
  install the missing package from the list above.
- **`synth` not runnable from a terminal (Linux)** — `~/.local/bin` isn't on your `PATH`; add
  `export PATH="$HOME/.local/bin:$PATH"` to `~/.bashrc` (or use the menu entry).
- **No sound (standalone, Linux)** — open Audio/MIDI Settings and pick the `pipewire`/
  `default` device; check `~/.config/synth/synth.log` for the device list it saw.
- **Windows SmartScreen** warns on the unsigned standalone `.exe` — "More info → Run anyway"
  (it's your own local build).
