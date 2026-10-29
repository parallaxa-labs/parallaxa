<#
.SYNOPSIS
  Installs the freshly built Parallaxa plugin for local testing in OBS.

.DESCRIPTION
  Copies build/parallaxa.dll (+ .pdb) and data into
  C:\ProgramData\obs-studio\plugins\parallaxa using BOTH layouts, which
  share one data folder:

    parallaxa\parallaxa.dll          OBS 33+ ("flat", loaded first;
                                               33+ then skips the legacy copy)
    parallaxa\bin\64bit\parallaxa.dll OBS 32.x (legacy layout)
    parallaxa\data\...                    locale, effects, presets

  Steam updates OBS silently (32 -> 33), so writing both keeps the plugin
  loading across that update. The installed OBS version (obs64.exe
  FileVersion) is reported so you know which copy will be loaded. Never
  writes into the OBS (Steam) program folder. Close OBS first: a loaded DLL
  cannot be replaced.

  Windows PowerShell 5.1 compatible. Supports -WhatIf.

.EXAMPLE
  powershell -NoProfile -File tools/install-dev.ps1
  powershell -NoProfile -File tools/install-dev.ps1 -Layout Flat
  powershell -NoProfile -File tools/install-dev.ps1 -Uninstall
#>
# Copyright (C) 2026 Parallaxa Labs
# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$BuildDir,
    [string]$ObsDir,
    # Parent "plugins" folder. Default: %ProgramData%\obs-studio\plugins
    [string]$PluginsDir,
    # Both (default) | Flat (33+ only) | Legacy (bin\64bit only)
    [ValidateSet('Both', 'Flat', 'Legacy')]
    [string]$Layout = 'Both',
    [switch]$Uninstall,
    [switch]$Force
)

. "$PSScriptRoot\common.ps1"

$ModuleName = 'parallaxa'
if (-not $BuildDir) { $BuildDir = Join-Path $script:PluginDir 'build' }
if (-not $ObsDir) { $ObsDir = $script:DefaultObsDir }
if (-not $PluginsDir) { $PluginsDir = Join-Path $env:ProgramData 'obs-studio\plugins' }

function Get-FullPath([string]$p) {
    return [System.IO.Path]::GetFullPath($p).TrimEnd('\')
}

$target = Join-Path $PluginsDir $ModuleName
$flatDir = $target
$legacyDir = Join-Path $target 'bin\64bit'
$dataDir = Join-Path $target 'data'

# Safety: never write into an OBS program folder (Steam verify/updates wipe
# it). Checked against -ObsDir AND the detected default OBS folder, so passing a
# different -ObsDir cannot open the door, and against any Steam library.
$tgtFull = Get-FullPath $target
foreach ($guard in @($ObsDir, $script:DefaultObsDir)) {
    if (-not $guard) { continue }
    $gFull = Get-FullPath $guard
    if ($tgtFull.Equals($gFull, [System.StringComparison]::OrdinalIgnoreCase) -or
        $tgtFull.StartsWith($gFull + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to install into the OBS program folder ($gFull)."
    }
}
if ($tgtFull -match '\\steamapps\\common\\') {
    throw "Refusing to install into a Steam library folder ($tgtFull)."
}

# OBS keeps the DLL locked while running.
$running = Get-Process -Name 'obs64' -ErrorAction SilentlyContinue
if ($running -and -not $Force -and -not $WhatIfPreference) {
    throw 'OBS is running. Close it first (or pass -Force to try anyway).'
}

$obsVersion = $null
try { $obsVersion = Get-ObsVersion $ObsDir } catch { Write-Warning $_.Exception.Message }

function Remove-PluginFile([string]$path) {
    if (Test-Path $path) {
        if ($PSCmdlet.ShouldProcess($path, 'Remove')) { Remove-Item -Force $path }
    }
}

function Remove-EmptyDir([string]$path) {
    if ((Test-Path $path) -and -not (Get-ChildItem -Force $path)) {
        if ($PSCmdlet.ShouldProcess($path, 'Remove empty folder')) { Remove-Item -Force $path }
    }
}

$binaries = @("$ModuleName.dll", "$ModuleName.pdb")

if ($Uninstall) {
    Write-Step "Uninstalling $target"
    if (Test-Path $target) {
        if ($PSCmdlet.ShouldProcess($target, 'Remove plugin folder')) {
            Remove-Item -Recurse -Force $target
        }
    }
    Write-Step 'OK'
    exit 0
}

$dll = Join-Path $BuildDir "$ModuleName.dll"
if (-not (Test-Path $dll)) { throw "$dll not found. Run tools/build.ps1 first." }
$srcData = Join-Path $script:PluginDir 'data'

$writeFlat = $Layout -ne 'Legacy'
$writeLegacy = $Layout -ne 'Flat'

Write-Step "Installing $ModuleName ($Layout layout) into $target"

# Binaries
foreach ($pair in @(@($writeFlat, $flatDir), @($writeLegacy, $legacyDir))) {
    $want = $pair[0]
    $dir = $pair[1]
    if ($want) {
        if ($PSCmdlet.ShouldProcess($dir, 'Copy parallaxa.dll/.pdb')) {
            New-Item -ItemType Directory -Force -Path $dir | Out-Null
            foreach ($b in $binaries) {
                $src = Join-Path $BuildDir $b
                if (Test-Path $src) { Copy-Item -Force $src (Join-Path $dir $b) }
            }
        }
    } else {
        # Layout not requested: remove a stale copy so OBS cannot load an old build.
        foreach ($b in $binaries) { Remove-PluginFile (Join-Path $dir $b) }
    }
}
if (-not $writeLegacy) {
    Remove-EmptyDir $legacyDir
    Remove-EmptyDir (Join-Path $target 'bin')
}

# Data: mirror data (drop files that no longer exist in the source).
if (Test-Path $srcData) {
    if ($PSCmdlet.ShouldProcess($dataDir, 'Mirror data')) {
        if (Test-Path $dataDir) { Remove-Item -Recurse -Force $dataDir }
        New-Item -ItemType Directory -Force -Path $dataDir | Out-Null
        Copy-Item -Recurse -Force (Join-Path $srcData '*') $dataDir
    }
}

if ($obsVersion) {
    if ($obsVersion.Major -ge 33) {
        $which = if ($writeFlat) { 'flat (parallaxa.dll)' } else { 'NONE: OBS 33+ needs the flat layout or legacy fallback' }
        if (-not $writeFlat -and $writeLegacy) { $which = 'legacy bin\64bit (shown as "Legacy" in OBS 33+)' }
    } else {
        $which = if ($writeLegacy) { 'legacy bin\64bit' } else { 'NONE: OBS 32.x needs bin\64bit (use -Layout Both)' }
    }
    Write-Step "OBS $obsVersion will load: $which"
}
Write-Step 'OK. Start OBS and look for "[parallaxa] loaded v" in the log.'
