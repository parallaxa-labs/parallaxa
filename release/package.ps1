<#
.SYNOPSIS
  Builds and packages a Parallaxa release into dist/release/: the plugin zip
  (both OBS layouts), the PDB zip, the GPL source zip and, when Inno Setup is
  available, the installer.

.DESCRIPTION
  Steps:
    1. VS 2022 x64 developer shell, pinned to the Windows SDK of
       buildspec.json when it is installed.
    2. OBS headers (buildspec "obs-studio" tag) and x64 import libraries
       (with -Ci: from the official OBS zip in buildspec.json, SHA-256 checked;
       locally: from -ObsDir or the default OBS folder), then tools/build.ps1
       (build + unit tests) into dist/build/plugin.
    3. Stage dist/release/stage/parallaxa: parallaxa.dll in both layouts
       (flat + bin\64bit), data\, LICENSE.txt, NOTICE.txt, THIRD_PARTY_NOTICES.md,
       licenses\, SOURCE.txt.
    4. With -Ci or -Smoke: release/obs-smoke.ps1 loads the DLL into the
       official OBS 32.0.0 and 33.0.0 builds.
    5. Zips. The source zip is the complete corresponding source: this whole
       repository (plugin sources, build and release scripts, docs/protocol,
       dock-src/ with the source of data/dock/dock-standalone.html). From a
       clean git checkout it is `git archive HEAD`; uncommitted changes (or no
       git checkout at all) stop the script unless -AllowDirtySource, which
       exports the working tree and says so in SOURCE.txt.
    6. Optional Authenticode signing (release/sign.ps1, env secrets) and the
       Inno Setup installer (release/stage.iss). Without ISCC the installer
       is skipped with a warning (-RequireInstaller makes that an error).
    7. dist/release/SHA256SUMS.txt.

  Writes only under dist/ and the build folders the tool scripts own
  (third_party/obs-src, third_party/implibs). Windows PowerShell 5.1
  compatible.

.EXAMPLE
  powershell -NoProfile -File release/package.ps1 -AllowDirtySource
  powershell -NoProfile -File release/package.ps1 -Ci -RequireInstaller -SourceUrl <public repo URL>   # CI

  -SourceUrl is the public repository named in SOURCE.txt (the GPL source
  pointer), the installer and the About row of the plugin. It defaults to an
  example.com placeholder; -Ci refuses the placeholder.
#>
[CmdletBinding()]
param(
    [string]$Version,                       # default: project(VERSION) in CMakeLists.txt
    [switch]$Ci,                            # import libs from the official OBS zip in buildspec.json; smoke test
    [string]$ObsDir,                        # local OBS for import libs (default: tools/common.ps1)
    [switch]$SkipBuild,                     # reuse dist/build/plugin (or build/)
    [switch]$NoTests,
    [switch]$Smoke, [switch]$NoSmoke,
    [switch]$NoInstaller,
    [switch]$RequireInstaller,
    [string]$IsccPath,
    [string]$SourceUrl = 'https://example.com/parallaxa',   # public repository (SOURCE.txt, installer, About row); CI passes the real one
    [switch]$AllowDirtySource,
    [switch]$RequireSigning                 # also: SIGN_REQUIRED=1
)
Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Rel = Join-Path $Repo 'dist\release'
$BuildRoot = Join-Path $Repo 'dist\build'
$Cache = Join-Path $Repo 'dist\cache'
$Spec = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'buildspec.json') -Raw | ConvertFrom-Json
$PowerShellExe = (Get-Process -Id $PID).Path
$Tar = Join-Path $env:SystemRoot 'System32\tar.exe'

# Never part of the source zip: build outputs and fetched third-party trees.
$SourceExclude = @('.git', 'build', 'dist', 'node_modules', 'third_party/obs-src', 'third_party/implibs', 'tools/__pycache__')

function Step([string]$m) { Write-Host "==> $m" -ForegroundColor Cyan }

function Invoke-Script([string]$Path, [string[]]$Arguments = @()) {
    & $PowerShellExe -NoProfile -File $Path @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $Path) failed (exit $LASTEXITCODE)" }
}

function Invoke-Native([string]$Exe, [string[]]$Arguments = @(), [string]$Dir = $Repo) {
    Push-Location $Dir
    try {
        & $Exe @Arguments
        if ($LASTEXITCODE -ne 0) { throw "$Exe $($Arguments -join ' ') failed (exit $LASTEXITCODE)" }
    } finally { Pop-Location }
}

function New-CleanDir([string]$Path) {
    if (Test-Path -LiteralPath $Path) { Remove-Item -LiteralPath $Path -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $Path | Out-Null
}

# Runs git with ErrorActionPreference=Continue (Windows PowerShell turns any
# stderr line of a native command into an error under Stop).
function Invoke-Git([string[]]$GitArgs) {
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & git -C $Repo @GitArgs 2>$null
        return [pscustomobject]@{ Code = $LASTEXITCODE; Out = @($out) }
    } finally { $ErrorActionPreference = $old }
}

# True when this folder is the top level of its own git repository (not a
# folder inside another checkout).
function Test-OwnGitRepo {
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) { return $false }
    $r = Invoke-Git @('rev-parse', '--show-toplevel')
    if ($r.Code -ne 0 -or -not $r.Out) { return $false }
    return ((Resolve-Path -LiteralPath $r.Out[0]).Path.TrimEnd('\') -eq $Repo.TrimEnd('\'))
}

# "0.1.0-dev+abc" -> "0.1.0" (Windows version resources need numbers).
function Get-NumericVersion([string]$v) {
    $m = [regex]::Match($v, '^\d+(\.\d+){0,3}')
    if (-not $m.Success) { throw "version '$v' does not start with a number" }
    return $m.Value
}

function Get-VsDir {
    $default = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools'
    if (Test-Path (Join-Path $default 'Common7\Tools\Launch-VsDevShell.ps1')) { return $default }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $p = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($p) { return $p }
    }
    throw 'Visual Studio 2022 with the C++ workload was not found'
}

# Enters the x64 developer shell in this process, pinned to the buildspec SDK;
# the child scripts inherit it.
function Enter-VsShell {
    if ($env:VSCMD_ARG_TGT_ARCH -eq 'x64' -and (Get-Command cl.exe -ErrorAction SilentlyContinue)) { return }
    $vs = Get-VsDir
    $sdk = $Spec.platformConfig.'windows-x64'.windowsSdk
    $devArgs = ''
    if (Test-Path (Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\Include\$sdk")) { $devArgs = "-winsdk=$sdk" }
    else { Write-Warning "Windows SDK $sdk is not installed; using the newest SDK" }
    $installer = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'
    if (Test-Path $installer) { $env:PATH = "$installer;$env:PATH" }
    Step "VS developer shell ($vs, x64, SDK $sdk)"
    $here = Get-Location
    $mod = Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
    if (-not (Test-Path -LiteralPath $mod)) { $mod = Join-Path $vs 'Common7\Tools\vsdevshell\Microsoft.VisualStudio.DevShell.dll' }
    Import-Module $mod
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments ("-arch=x64 -host_arch=x64 " + $devArgs).Trim() | Out-Null
    Set-Location $here
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw 'cl.exe missing after entering the VS developer shell' }
}

function Find-Iscc {
    if ($IsccPath) { if (Test-Path -LiteralPath $IsccPath) { return $IsccPath } else { throw "ISCC not found: $IsccPath" } }
    $c = Get-Command iscc.exe -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    foreach ($p in @(
            (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
            (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe'),
            (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'))) {
        if (Test-Path -LiteralPath $p) { return $p }
    }
    return $null
}

# Downloads the official OBS Windows zip of buildspec "obs-binaries" (SHA-256
# pinned) and extracts only the DLLs the import libraries are made from.
function Get-ObsBinaries {
    $b = $Spec.dependencies.'obs-binaries'
    $dir = Join-Path $Cache "obs-$($b.version)"
    if (Test-Path -LiteralPath (Join-Path $dir 'bin\64bit\obs.dll')) { return $dir }
    New-Item -ItemType Directory -Force -Path $Cache | Out-Null
    $zip = Join-Path $Cache (Split-Path -Leaf $b.url)
    $ok = (Test-Path -LiteralPath $zip) -and ((Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash.ToLowerInvariant() -eq $b.sha256)
    if (-not $ok) {
        Step "download $($b.url)"
        & (Join-Path $env:SystemRoot 'System32\curl.exe') --fail --location --silent --show-error --retry 3 --output "$zip.part" $b.url
        if ($LASTEXITCODE -ne 0) { throw "download failed ($LASTEXITCODE)" }
        $h = (Get-FileHash -Algorithm SHA256 -LiteralPath "$zip.part").Hash.ToLowerInvariant()
        if ($h -ne $b.sha256) { Remove-Item -LiteralPath "$zip.part" -Force; throw "SHA-256 mismatch for $($b.url): $h" }
        Move-Item -LiteralPath "$zip.part" -Destination $zip -Force
    }
    New-CleanDir $dir
    Invoke-Native $Tar (@('-xf', $zip, '-C', $dir) + @($b.files))
    return $dir
}

# Repository-relative paths (forward slashes) of the working tree, minus
# $SourceExclude and, in a git checkout, minus .gitignore'd files.
function Get-WorkingTreeFiles([bool]$Git) {
    if ($Git) {
        $r = Invoke-Git @('ls-files', '-c', '-o', '--exclude-standard')
        if ($r.Code -ne 0) { throw "git ls-files failed ($($r.Code))" }
        $files = $r.Out
    } else {
        # Walk by hand so excluded folders (node_modules with its links) are never entered.
        $files = New-Object System.Collections.Generic.List[string]
        $stack = New-Object System.Collections.Generic.Stack[string]
        $stack.Push('')
        while ($stack.Count -gt 0) {
            $relDir = $stack.Pop()
            foreach ($e in Get-ChildItem -LiteralPath (Join-Path $Repo $relDir) -Force) {
                $p = if ($relDir) { "$relDir/$($e.Name)" } else { $e.Name }
                if ($SourceExclude | Where-Object { $p -eq $_ -or $e.Name -eq $_ }) { continue }
                if ($e.PSIsContainer) { $stack.Push($p) } else { $files.Add($p) }
            }
        }
    }
    return @($files | Where-Object {
            $f = $_
            -not ($SourceExclude | Where-Object { $f -eq $_ -or $f.StartsWith("$_/") -or $f.Contains("/$_/") }) -and
            (Test-Path -LiteralPath (Join-Path $Repo $f) -PathType Leaf)
        })
}

# Writes the complete corresponding source into $Dest. Returns a note for
# SOURCE.txt when the export is not a clean commit.
function Export-Source([string]$Dest, [bool]$Git, [string[]]$Dirt) {
    New-CleanDir $Dest
    if ($Git -and $Dirt.Count -eq 0) {
        $tarFile = Join-Path $BuildRoot 'source.tar'
        $r = Invoke-Git @('archive', '--format=tar', '-o', $tarFile, 'HEAD')
        if ($r.Code -ne 0) { throw "git archive failed ($($r.Code))" }
        Invoke-Native $Tar @('-xf', $tarFile, '-C', $Dest)
        Remove-Item -LiteralPath $tarFile -Force
        return ''
    }
    foreach ($f in Get-WorkingTreeFiles $Git) {
        $to = Join-Path $Dest $f
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $to) | Out-Null
        Copy-Item -LiteralPath (Join-Path $Repo $f) -Destination $to -Force
    }
    if ($Git) { return "NOTE: development build from a working tree with $($Dirt.Count) uncommitted change(s); the source archive holds that working tree." }
    return 'NOTE: development build from a folder that is not a git checkout; the source archive holds that folder.'
}

# ---------------------------------------------------------------------------
if (-not $Version) {
    $m = [regex]::Match((Get-Content -LiteralPath (Join-Path $Repo 'CMakeLists.txt') -Raw), 'project\([^)]*VERSION\s+([0-9.]+)')
    $Version = $m.Groups[1].Value
}
$NumVersion = Get-NumericVersion $Version
$git = Test-OwnGitRepo
$commit = 'unknown'
$dirt = @()
if ($git) {
    $c = Invoke-Git @('rev-parse', 'HEAD')
    if ($c.Code -eq 0 -and $c.Out) { $commit = $c.Out[0] }
    $dirt = @((Invoke-Git @('status', '--porcelain')).Out | Where-Object { $_ })
}
# SOURCE.txt is the GPL pointer to the source: a CI (release) build must name
# the real public repository, never the placeholder.
if ($SourceUrl -notmatch '^https://[^"\s\\]+$') { throw "-SourceUrl must be an https:// URL: $SourceUrl" }
$SourceUrl = $SourceUrl.TrimEnd('/')
if ($SourceUrl -match '^https://([^/]+\.)?example\.(com|org|net)(/|$)') {
    if ($Ci) { throw "-Ci build without a real -SourceUrl ($SourceUrl is a placeholder): pass the public repository URL" }
    Write-Warning "SOURCE.txt, the installer and the About row point at the placeholder $SourceUrl (pass -SourceUrl for a release)"
}
if ((-not $git -or $dirt.Count -gt 0) -and -not $AllowDirtySource) {
    $why = if ($git) { "uncommitted changes:`n  " + (($dirt | Select-Object -First 15) -join "`n  ") } else { "$Repo is not a git checkout" }
    throw "the GPL source zip must match the binaries, but $why`nCommit first, or pass -AllowDirtySource for a development build."
}
Step "Parallaxa $Version (commit $commit)"
if ($env:SIGN_REQUIRED -eq '1') { $RequireSigning = $true }
$signing = [bool]($env:SIGN_PFX_BASE64 -or $env:SIGN_PFX_PATH)
if ($RequireSigning -and -not $signing) { throw 'signing is required but no certificate is configured (SIGN_PFX_BASE64 + SIGN_PFX_PASSWORD, or SIGN_PFX_PATH)' }

New-Item -ItemType Directory -Force -Path $Rel, $BuildRoot | Out-Null
Enter-VsShell
$outputs = New-Object System.Collections.Generic.List[string]

# ---- build -------------------------------------------------------------------
$pluginBuild = Join-Path $BuildRoot 'plugin'
$tools = Join-Path $Repo 'tools'
if (-not $SkipBuild) {
    $tag = $Spec.dependencies.'obs-studio'.version
    Step "OBS $tag headers"
    Invoke-Script (Join-Path $tools 'fetch-obs-headers.ps1') @('-Tag', $tag)
    $implibs = Join-Path $Repo 'third_party\implibs'
    if ($Ci) {
        $obs = Get-ObsBinaries
        Step "import libraries from OBS $($Spec.dependencies.'obs-binaries'.version)"
        Invoke-Script (Join-Path $tools 'make-implibs.ps1') @('-ObsDir', $obs)
    } elseif (-not (Test-Path -LiteralPath (Join-Path $implibs 'obs.lib')) -or $ObsDir) {
        $a = @(); if ($ObsDir) { $a = @('-ObsDir', $ObsDir) }
        Step 'import libraries from the local OBS'
        Invoke-Script (Join-Path $tools 'make-implibs.ps1') $a
    }
    Step 'build parallaxa.dll'
    $a = @('-Config', $Spec.platformConfig.'windows-x64'.config, '-BuildDir', $pluginBuild, '-ProjectUrl', $SourceUrl)
    if ($NoTests) { $a += '-NoTests' }
    Invoke-Script (Join-Path $tools 'build.ps1') $a
} elseif (-not (Test-Path -LiteralPath (Join-Path $pluginBuild 'parallaxa.dll'))) {
    $pluginBuild = Join-Path $Repo 'build'   # tools/build.ps1's default folder
    Write-Warning "-SkipBuild: using $pluginBuild"
}
$dll = Join-Path $pluginBuild 'parallaxa.dll'
if (-not (Test-Path -LiteralPath $dll)) { throw "$dll not found (build the plugin first)" }
if ($signing) {
    Step 'sign parallaxa.dll'
    $a = @($dll); if ($RequireSigning) { $a += '-Require' }
    Invoke-Script (Join-Path $PSScriptRoot 'sign.ps1') $a
}

# ---- stage -------------------------------------------------------------------
Step 'stage the plugin package'
$stageRoot = Join-Path $Rel 'stage'
$pkg = Join-Path $stageRoot 'parallaxa'
New-CleanDir $stageRoot
New-Item -ItemType Directory -Force -Path (Join-Path $pkg 'bin\64bit') | Out-Null
foreach ($d in @($pkg, (Join-Path $pkg 'bin\64bit'))) {
    Copy-Item -LiteralPath $dll -Destination $d
    $pdb = Join-Path $pluginBuild 'parallaxa.pdb'
    if (Test-Path -LiteralPath $pdb) { Copy-Item -LiteralPath $pdb -Destination $d }
}
Copy-Item -LiteralPath (Join-Path $Repo 'data') -Destination (Join-Path $pkg 'data') -Recurse
Copy-Item -LiteralPath (Join-Path $Repo 'LICENSE') -Destination (Join-Path $pkg 'LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $Repo 'NOTICE') -Destination (Join-Path $pkg 'NOTICE.txt')
Copy-Item -LiteralPath (Join-Path $Repo 'THIRD_PARTY_NOTICES.md') -Destination $pkg
Copy-Item -LiteralPath (Join-Path $Repo 'licenses') -Destination (Join-Path $pkg 'licenses') -Recurse

$srcRoot = Join-Path $BuildRoot 'source'
$srcPkg = Join-Path $srcRoot "parallaxa-$Version"
$note = Export-Source $srcPkg $git $dirt
$sourceTxt = @"
Parallaxa $Version
Copyright (C) 2026 Parallaxa Labs

Parallaxa is free software: you can redistribute it and/or modify it
under the terms of the GNU General Public License, version 2 or (at your
option) any later version (LICENSE.txt). It comes with ABSOLUTELY NO WARRANTY.

Complete corresponding source code
- Repository: $SourceUrl
- This build: version $Version, tag v$Version, commit $commit
- Source archive: Parallaxa-$Version-source.zip, published next to
  this installer and zip on the release page.
The source includes the build scripts (tools/), the release and installer
scripts (release/), the obs-websocket protocol documentation (docs/protocol)
and, in dock-src/, the Svelte source and build configuration (package.json,
pnpm-lock.yaml, vite.config.ts) of data/dock/dock-standalone.html, which is a
minified build. dock-src/README.md explains how to rebuild the dock.

Install layouts
- OBS 33 and later load  %ProgramData%\obs-studio\plugins\parallaxa\parallaxa.dll
- OBS 32 loads           %ProgramData%\obs-studio\plugins\parallaxa\bin\64bit\parallaxa.dll
Both share %ProgramData%\obs-studio\plugins\parallaxa\data\.
"@
if ($note) { $sourceTxt += "`r`n$note`r`n" }
$utf8 = New-Object Text.UTF8Encoding($false)
[IO.File]::WriteAllText((Join-Path $pkg 'SOURCE.txt'), ($sourceTxt -replace "`r?`n", "`r`n"), $utf8)
Copy-Item -LiteralPath (Join-Path $pkg 'SOURCE.txt') -Destination $srcPkg

if (($Ci -or $Smoke) -and -not $NoSmoke) {
    Step 'OBS smoke test (imports + headless load on every buildspec obsSmoke build)'
    Invoke-Script (Join-Path $PSScriptRoot 'obs-smoke.ps1') @('-PluginDir', $pkg, '-Cache', $Cache)
}

# ---- zips --------------------------------------------------------------------
Step 'zips'
$zip = Join-Path $Rel "Parallaxa-$Version-windows-x64.zip"
$pdbZip = Join-Path $Rel "Parallaxa-$Version-pdb.zip"
$srcZip = Join-Path $Rel "Parallaxa-$Version-source.zip"
foreach ($z in @($zip, $pdbZip, $srcZip)) { if (Test-Path -LiteralPath $z) { Remove-Item -LiteralPath $z -Force } }
$listFile = Join-Path $BuildRoot 'zip.lst'
$zipName = { param($f) 'parallaxa/' + $f.FullName.Substring($pkg.Length + 1).Replace('\', '/') }
$zipList = @(Get-ChildItem -LiteralPath $pkg -Recurse -File | Where-Object { $_.Extension -ne '.pdb' } | ForEach-Object { & $zipName $_ })
[IO.File]::WriteAllLines($listFile, [string[]]$zipList)
Invoke-Native $Tar @('-a', '-c', '-f', $zip, '-T', $listFile) $stageRoot
$outputs.Add($zip)
$pdbList = @(Get-ChildItem -LiteralPath $pkg -Recurse -File -Filter '*.pdb' | ForEach-Object { & $zipName $_ })
if ($pdbList.Count -gt 0) {
    [IO.File]::WriteAllLines($listFile, [string[]]$pdbList)
    Invoke-Native $Tar @('-a', '-c', '-f', $pdbZip, '-T', $listFile) $stageRoot
    $outputs.Add($pdbZip)
}
Invoke-Native $Tar @('-a', '-c', '-f', $srcZip, "parallaxa-$Version") $srcRoot
$outputs.Add($srcZip)

# ---- installer -----------------------------------------------------------------
if (-not $NoInstaller) {
    $iscc = Find-Iscc
    if (-not $iscc) {
        $msg = 'Inno Setup 6.3+ (ISCC.exe) not found: installer skipped (https://jrsoftware.org/isdl.php, or pass -IsccPath)'
        if ($RequireInstaller) { throw $msg } else { Write-Warning $msg }
    } else {
        $out = Join-Path $Rel 'installers'
        New-Item -ItemType Directory -Force -Path $out | Out-Null
        $a = @('/Q', "/DAppVersion=$Version", "/DNumericVersion=$NumVersion", "/DStageDir=$pkg", "/DOutputDir=$out", "/DSourceURL=$SourceUrl")
        if ($signing) {
            # $q is Inno's quote character, $f the quoted file to sign.
            $signCmd = "`$q$PowerShellExe`$q -NoProfile -File `$q$(Join-Path $PSScriptRoot 'sign.ps1')`$q `$f"
            $a += @("/Sparallaxa=$signCmd", '/DSignTool=parallaxa')
        }
        Step 'Inno Setup: stage.iss'
        Invoke-Native $iscc ($a + @((Join-Path $PSScriptRoot 'stage.iss')))
        $outputs.Add((Join-Path $out "Parallaxa-$Version-Setup.exe"))
    }
}

# ---- checksums -------------------------------------------------------------------
$sums = foreach ($o in $outputs) {
    '{0}  {1}' -f (Get-FileHash -Algorithm SHA256 -LiteralPath $o).Hash.ToLowerInvariant(), $o.Substring($Rel.Length + 1).Replace('\', '/')
}
[IO.File]::WriteAllLines((Join-Path $Rel 'SHA256SUMS.txt'), [string[]]$sums)
Step 'outputs'
foreach ($o in $outputs) { Write-Host ('    {0,10:N0} KB  {1}' -f ((Get-Item -LiteralPath $o).Length / 1KB), $o) }
Step 'done'
