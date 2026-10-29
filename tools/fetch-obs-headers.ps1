<#
.SYNOPSIS
  Sparse, shallow clone of obsproject/obs-studio (headers only) into
  third_party/obs-src.

.DESCRIPTION
  Checks out only libobs, frontend/api and deps/w32-pthreads at the given tag.
  Headers are pinned to the OLDEST supported OBS version (32.0.0) so the plugin
  only uses API that exists on every supported OBS; the import libraries come
  from the installed OBS (make-implibs.ps1). Nothing here builds libobs.

  Windows PowerShell 5.1 compatible.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools/fetch-obs-headers.ps1 -Tag 32.0.0
#>
# Copyright (C) 2026 Parallaxa Labs
# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param(
    [string]$Tag = '32.0.0',
    [string]$Dest,
    [string]$Repo = 'https://github.com/obsproject/obs-studio.git',
    [switch]$Force
)

. "$PSScriptRoot\common.ps1"

if (-not $Dest) { $Dest = Join-Path $script:PluginDir 'third_party\obs-src' }
$git = (Get-Command git.exe -ErrorAction SilentlyContinue)
if (-not $git) { throw 'git is required (https://git-scm.com).' }
$git = $git.Source

$stamp = Join-Path $Dest '.stage-tag'
if ((Test-Path $stamp) -and -not $Force) {
    $have = (Get-Content $stamp -Raw).Trim()
    if ($have -eq $Tag -and (Test-Path (Join-Path $Dest 'libobs\obs-module.h'))) {
        Write-Step "obs-studio $Tag headers already present in $Dest (use -Force to refetch)"
        exit 0
    }
}

if (Test-Path $Dest) {
    Write-Step "Removing old $Dest"
    Remove-Item -Recurse -Force $Dest
}

Write-Step "Sparse clone of obs-studio tag $Tag"
# Blob-less, depth-1, cone-mode sparse checkout: a few MB instead of ~1 GB.
Invoke-Native $git @('-c', 'advice.detachedHead=false', 'clone', '--depth', '1', '--branch', $Tag,
    '--filter=blob:none', '--sparse', '--no-tags', $Repo, $Dest)
Invoke-Native $git @('-C', $Dest, 'sparse-checkout', 'set', 'libobs', 'frontend/api', 'deps/w32-pthreads')

foreach ($p in @('libobs\obs-module.h', 'frontend\api\obs-frontend-api.h', 'deps\w32-pthreads\pthread.h')) {
    if (-not (Test-Path (Join-Path $Dest $p))) { throw "Expected header missing after clone: $p" }
}
Set-Content -Path $stamp -Value $Tag -Encoding ASCII
Write-Step "OK: obs-studio $Tag headers in $Dest"
