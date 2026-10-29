<#
.SYNOPSIS
  Generates x64 import libraries (obs.lib, obs-frontend-api.lib,
  w32-pthreads.lib) from an installed OBS Studio, so the plugin links without
  building libobs.

.DESCRIPTION
  Enters the VS developer shell (Launch-VsDevShell.ps1 -Arch amd64), runs
  dumpbin /exports on <ObsDir>\bin\64bit\{obs,obs-frontend-api,w32-pthreads}.dll,
  writes a .def per DLL and runs lib /def /machine:x64 into
  third_party/implibs. x64 C exports are undecorated, so .def -> lib is
  exact. Import libs from a newer OBS are a superset of the 32.0.0 headers' API.

  Without -ObsDir it uses OBS_DIR or the OBS install release/obs-detect.ps1
  finds (tools/common.ps1). Read-only on the OBS folder. Windows PowerShell
  5.1 compatible.

.EXAMPLE
  powershell -NoProfile -File tools/make-implibs.ps1 -ObsDir "C:\Program Files\obs-studio"
#>
# Copyright (C) 2026 Parallaxa Labs
# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param(
    [string]$ObsDir,
    [string]$OutDir,
    [string]$VsDir
)

. "$PSScriptRoot\common.ps1"

if (-not $ObsDir) { $ObsDir = $script:DefaultObsDir }
if (-not $OutDir) { $OutDir = Join-Path $script:PluginDir 'third_party\implibs' }
$binDir = Join-Path $ObsDir 'bin\64bit'
if (-not (Test-Path (Join-Path $binDir 'obs.dll'))) {
    throw "obs.dll not found in $binDir. Pass -ObsDir <OBS install folder>."
}

Enter-VsDev $VsDir
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# Parses `dumpbin /exports` output into a list of exported names.
function Get-DllExports([string]$Dll) {
    $lines = & dumpbin.exe /NOLOGO /EXPORTS $Dll
    if ($LASTEXITCODE -ne 0) { throw "dumpbin failed on $Dll" }
    $names = New-Object System.Collections.Generic.List[string]
    $inTable = $false
    foreach ($l in $lines) {
        if (-not $inTable) {
            if ($l -match '^\s+ordinal\s+hint\s+RVA\s+name') { $inTable = $true }
            continue
        }
        if ($l -match '^\s*Summary') { break }
        # "  12    B 0001A2C0 obs_source_create"  or forwarded/no-RVA rows
        # "  13    C          name (forwarded to X.y)"
        $m = [regex]::Match($l, '^\s+\d+\s+[0-9A-Fa-f]+\s+(?:[0-9A-Fa-f]{8}\s+)?([^\s=]+)')
        if ($m.Success) { $names.Add($m.Groups[1].Value) }
    }
    return $names
}

foreach ($name in @('obs', 'obs-frontend-api', 'w32-pthreads')) {
    $dll = Join-Path $binDir "$name.dll"
    if (-not (Test-Path $dll)) { throw "Missing $dll" }
    $exports = Get-DllExports $dll
    if ($exports.Count -lt 1) { throw "No exports parsed from $dll" }

    $def = Join-Path $OutDir "$name.def"
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine("LIBRARY `"$name.dll`"")
    [void]$sb.AppendLine('EXPORTS')
    foreach ($e in $exports) { [void]$sb.AppendLine("    $e") }
    [System.IO.File]::WriteAllText($def, $sb.ToString(), (New-Object System.Text.UTF8Encoding $false))

    $lib = Join-Path $OutDir "$name.lib"
    Invoke-Native 'lib.exe' @('/NOLOGO', "/DEF:$def", "/OUT:$lib", '/MACHINE:X64')
    Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $OutDir "$name.exp")
    Write-Step ("{0}.lib: {1} exports" -f $name, $exports.Count)
}

$ver = Get-ObsVersion $ObsDir
Set-Content -Path (Join-Path $OutDir 'obs-version.txt') -Value $ver.ToString() -Encoding ASCII
Write-Step "OK: import libs for OBS $ver in $OutDir"
