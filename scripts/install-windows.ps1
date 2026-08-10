<#
.SYNOPSIS
  synth - Windows install script. Builds the Release binaries from source and installs
  the VST3 (per-user, no admin) + the standalone app with a Start Menu shortcut.

.DESCRIPTION
  Prerequisites (install once):
    * Visual Studio 2022 with the "Desktop development with C++" workload
      (or the standalone Build Tools for Visual Studio 2022)
    * CMake 3.22+  (winget install Kitware.CMake)  and Git
  Run from a normal PowerShell in the repo root:
      .\scripts\install-windows.ps1
  JUCE is fetched automatically by CMake on the first configure.

.PARAMETER Vst3Dir
  Where to install the VST3. Default is the per-user VST3 folder (no admin needed);
  most DAWs scan it. Use "$env:CommonProgramFiles\VST3" for the system-wide dir
  (that one needs an elevated/admin PowerShell).

.PARAMETER NoBuild
  Skip configure/build and just install whatever is already in the build dir.

.EXAMPLE
  .\scripts\install-windows.ps1
.EXAMPLE
  .\scripts\install-windows.ps1 -Vst3Dir "$env:CommonProgramFiles\VST3"   # run as admin
#>
#Requires -Version 5
param(
  [string]$Vst3Dir    = "$env:LOCALAPPDATA\Programs\Common\VST3",
  [string]$InstallDir = "$env:LOCALAPPDATA\Programs\synth",
  [string]$BuildDir   = "build",
  [switch]$NoBuild,
  [ValidateSet("Release","Debug","RelWithDebInfo")] [string]$Config = "Release"
)
$ErrorActionPreference = "Stop"
function Say($m)  { Write-Host "==> $m" -ForegroundColor Cyan }
function Warn($m) { Write-Host " warning: $m" -ForegroundColor Yellow }

$Repo = Split-Path -Parent $PSScriptRoot
Set-Location $Repo

# --- tool check -------------------------------------------------------------
foreach ($t in @("cmake","git")) {
  if (-not (Get-Command $t -ErrorAction SilentlyContinue)) {
    throw "$t not found on PATH. Install it (see the header of this script) and retry."
  }
}

# --- configure + build (multi-config VS generator) --------------------------
if (-not $NoBuild) {
  Say "Configuring in $BuildDir\ (first run fetches JUCE)…"
  cmake -B $BuildDir
  if ($LASTEXITCODE) { throw "cmake configure failed." }
  Say "Building $Config…"
  cmake --build $BuildDir --config $Config --target VASynth_Standalone VASynth_VST3
  if ($LASTEXITCODE) { throw "build failed." }
} else {
  Say "Skipping build (-NoBuild)."
}

$Art       = Join-Path $Repo "$BuildDir\VASynth_artefacts\$Config"
$ExeSrc    = Join-Path $Art "Standalone\synth.exe"
$Vst3Src   = Join-Path $Art "VST3\synth.vst3"
if (-not (Test-Path $ExeSrc))  { throw "standalone not built at $ExeSrc" }
if (-not (Test-Path $Vst3Src)) { throw "VST3 not built at $Vst3Src" }

# --- install VST3 -----------------------------------------------------------
Say "Installing VST3 -> $Vst3Dir\synth.vst3"
New-Item -ItemType Directory -Force -Path $Vst3Dir | Out-Null
$vst3Dst = Join-Path $Vst3Dir "synth.vst3"
if (Test-Path $vst3Dst) { Remove-Item -Recurse -Force $vst3Dst }
Copy-Item -Recurse -Force $Vst3Src $vst3Dst

# --- install standalone -----------------------------------------------------
Say "Installing standalone -> $InstallDir\synth.exe"
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
Copy-Item -Force $ExeSrc (Join-Path $InstallDir "synth.exe")

# --- Start Menu shortcut ----------------------------------------------------
try {
  $startMenu = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs"
  $lnk = Join-Path $startMenu "synth.lnk"
  $ws  = New-Object -ComObject WScript.Shell
  $sc  = $ws.CreateShortcut($lnk)
  $sc.TargetPath = Join-Path $InstallDir "synth.exe"
  $sc.WorkingDirectory = $InstallDir
  $sc.Description = "Virtual-analog polysynth (standalone)"
  $sc.Save()
  Say "Start Menu shortcut created."
} catch { Warn "could not create Start Menu shortcut: $_" }

Say "Done."
Write-Host "  Standalone : $InstallDir\synth.exe  (or the 'synth' Start Menu entry)"
Write-Host "  VST3       : $vst3Dst  (rescan plugins in your DAW)"
Write-Host "  Tip        : for lowest latency into your audio interface, load the VST3 in a DAW on the ASIO driver."
