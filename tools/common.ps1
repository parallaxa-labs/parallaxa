# Shared helpers for the Parallaxa plugin tool scripts.
# Windows PowerShell 5.1 compatible. Dot-source it: . "$PSScriptRoot\common.ps1"
#
# Copyright (C) 2026 Parallaxa Labs
# SPDX-License-Identifier: GPL-2.0-or-later

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

# The repository root (the parent of this tools/ folder).
$script:PluginDir = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
# OBS detection (registry, Steam libraries, Program Files): release/obs-detect.ps1.
. (Join-Path $script:PluginDir 'release\obs-detect.ps1')

# The installed OBS Studio used when a script gets no -ObsDir: the OBS_DIR
# environment variable, else the folder release/obs-detect.ps1 finds, else the
# default installer folder.
function Get-DefaultObsDir {
    if ($env:OBS_DIR) { return $env:OBS_DIR }
    $d = Find-ObsDir
    if ($d) { return $d }
    return (Join-Path $env:ProgramFiles 'obs-studio')
}
$script:DefaultObsDir = Get-DefaultObsDir
$script:DefaultVsDir = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools'

function Write-Step([string]$msg) {
    Write-Host "==> $msg" -ForegroundColor Cyan
}

# Returns the Visual Studio (Build Tools) installation folder.
function Get-VsInstallDir([string]$Hint) {
    if ($Hint -and (Test-Path (Join-Path $Hint 'Common7\Tools\Launch-VsDevShell.ps1'))) {
        return $Hint
    }
    if (Test-Path (Join-Path $script:DefaultVsDir 'Common7\Tools\Launch-VsDevShell.ps1')) {
        return $script:DefaultVsDir
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $p = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($p -and (Test-Path (Join-Path $p 'Common7\Tools\Launch-VsDevShell.ps1'))) { return $p }
    }
    throw 'Visual Studio 2022 (Build Tools) with the C++ workload was not found. Pass -VsDir.'
}

# Enters the x64 VS developer environment in the current PowerShell session
# (cl, link, lib, dumpbin, and the bundled cmake/ninja end up on PATH).
function Enter-VsDev([string]$VsDir) {
    if ($env:VSCMD_ARG_TGT_ARCH -eq 'x64' -and (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        return
    }
    $vs = Get-VsInstallDir $VsDir
    Write-Step "Entering VS dev shell ($vs, amd64)"
    $here = Get-Location
    # VsDevCmd.bat runs a bare "vswhere.exe" after pushd into the Installer
    # folder; with NoDefaultCurrentDirectoryInExePath set, cmd does not search
    # the current folder and prints "'vswhere.exe' is not recognized". Putting
    # the Installer folder on PATH avoids that harmless but noisy line.
    $installer = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'
    if ((Test-Path (Join-Path $installer 'vswhere.exe')) -and
        -not (($env:PATH -split ';') -contains $installer)) {
        $env:PATH = "$installer;$env:PATH"
    }
    & (Join-Path $vs 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
    Set-Location $here
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw 'cl.exe is not on PATH after entering the VS dev shell.'
    }
}

# The cmake.exe bundled with Visual Studio (the 3.31 the build is tested
# with). A cmake.exe on PATH is used only when the bundled one is missing, so
# a user-installed CMake cannot silently replace it.
function Get-CMakeExe {
    $vs = $null
    try { $vs = Get-VsInstallDir $null } catch { }
    if ($vs) {
        $p = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        if (Test-Path $p) { return $p }
    }
    $c = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($c) {
        Write-Warning "VS-bundled CMake not found; using $($c.Source)"
        return $c.Source
    }
    throw 'cmake.exe not found (install the VS "C++ CMake tools" component).'
}

function Get-CTestExe {
    $cm = Get-CMakeExe
    $p = Join-Path (Split-Path $cm) 'ctest.exe'
    if (Test-Path $p) { return $p }
    return 'ctest.exe'
}

# Runs a native command and throws when it exits non-zero.
function Invoke-Native {
    param([Parameter(Mandatory = $true)][string]$Exe, [string[]]$Arguments = @())
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Exe failed with exit code $LASTEXITCODE"
    }
}

# Reads "major.minor.patch" from obs64.exe's FileVersion, or from obs.dll's
# when only the DLLs are there (CI extracts just those from the OBS zip).
function Get-ObsVersion([string]$ObsDir) {
    $exe = Join-Path $ObsDir 'bin\64bit\obs64.exe'
    if (-not (Test-Path $exe)) { $exe = Join-Path $ObsDir 'bin\64bit\obs.dll' }
    if (-not (Test-Path $exe)) { throw "obs64.exe or obs.dll not found under $ObsDir" }
    $fv = (Get-Item $exe).VersionInfo.FileVersion
    if (-not $fv) { throw "obs64.exe has no FileVersion ($exe)" }
    $m = [regex]::Match($fv, '(\d+)\.(\d+)\.(\d+)')
    if (-not $m.Success) { throw "Unrecognized OBS FileVersion '$fv'" }
    return New-Object Version ([int]$m.Groups[1].Value), ([int]$m.Groups[2].Value), ([int]$m.Groups[3].Value)
}
