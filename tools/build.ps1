<#
.SYNOPSIS
  Configures and builds parallaxa.dll with the Visual Studio bundled
  CMake + Ninja (MSVC x64), then runs the unit tests.

.DESCRIPTION
  Prerequisites (one time): fetch-obs-headers.ps1 and make-implibs.ps1.
  Output: build/parallaxa.dll (+ .pdb). Windows PowerShell 5.1
  compatible.

.EXAMPLE
  powershell -NoProfile -File tools/build.ps1
  powershell -NoProfile -File tools/build.ps1 -Config Debug -Clean
#>
# Copyright (C) 2026 Parallaxa Labs
# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param(
    [ValidateSet('RelWithDebInfo', 'Release', 'Debug', 'MinSizeRel')]
    [string]$Config = 'RelWithDebInfo',
    [string]$BuildDir,
    [string]$ObsSrc,
    [string]$ImplibDir,
    [string]$ObsDir,                        # installed OBS for the headless tests (default: tools/common.ps1)
    [string]$VsDir,
    [string]$ProjectUrl,                    # -DSTAGE_PROJECT_URL (About row links; default: upstream repository)
    [switch]$Clean,
    [switch]$NoTests,
    # Size budget: the DLL (all modules) is 300 KB or smaller.
    # Optimized builds over budget fail unless -AllowOversize is passed.
    [int]$MaxDllKB = 300,
    [switch]$AllowOversize
)

. "$PSScriptRoot\common.ps1"

if (-not $BuildDir) { $BuildDir = Join-Path $script:PluginDir 'build' }

Enter-VsDev $VsDir
$cmake = Get-CMakeExe

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Step "Removing $BuildDir"
    Remove-Item -Recurse -Force $BuildDir
}

$cfgArgs = @('-S', $script:PluginDir, '-B', $BuildDir, '-G', 'Ninja', "-DCMAKE_BUILD_TYPE=$Config",
    '-DCMAKE_C_COMPILER=cl')
if ($ObsSrc) { $cfgArgs += "-DSTAGE_OBS_SRC=$ObsSrc" }
if ($ImplibDir) { $cfgArgs += "-DSTAGE_IMPLIB_DIR=$ImplibDir" }
if (-not $ObsDir) { $ObsDir = $script:DefaultObsDir }
if (Test-Path (Join-Path $ObsDir 'bin\64bit\obs.dll')) {
    $cfgArgs += "-DSTAGE_OBS_DIR=$($ObsDir.Replace('\', '/'))"
} else {
    Write-Warning "No OBS install at ${ObsDir}: the headless OBS tests are skipped (pass -ObsDir or set OBS_DIR)."
}
if ($ProjectUrl) { $cfgArgs += "-DSTAGE_PROJECT_URL=$ProjectUrl" }

Write-Step "Configure ($Config)"
Invoke-Native $cmake $cfgArgs
# The DLL first, so a broken unit test never blocks the plugin itself.
Write-Step 'Build parallaxa'
Invoke-Native $cmake @('--build', $BuildDir, '--parallel', '--target', 'parallaxa')
if (-not $NoTests) {
    Write-Step 'Build tests'
    Invoke-Native $cmake @('--build', $BuildDir, '--parallel')
}

$dll = Join-Path $BuildDir 'parallaxa.dll'
if (-not (Test-Path $dll)) { throw "Build finished but $dll is missing" }
$kb = [math]::Round((Get-Item $dll).Length / 1KB, 1)
Write-Step "parallaxa.dll: $kb KB ($dll)"
if ($Config -ne 'Debug' -and $kb -gt $MaxDllKB) {
    # Usual culprit: a large static array inside an initialised static struct
    # (e.g. one holding PTHREAD_MUTEX_INITIALIZER) lands in .data instead of
    # .bss. Check with: dumpbin /headers (.data "size of raw data").
    $msg = "parallaxa.dll is $kb KB, over the $MaxDllKB KB budget"
    if ($AllowOversize) { Write-Warning $msg } else { throw "$msg (pass -AllowOversize to ignore)" }
}

if (-not $NoTests) {
    Write-Step 'Unit tests'
    Invoke-Native (Get-CTestExe) @('--test-dir', $BuildDir, '--output-on-failure')
}
Write-Step 'OK'
