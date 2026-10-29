<#
.SYNOPSIS
  Release lint for Parallaxa that runs without Inno Setup, OBS or admin rights.

.DESCRIPTION
  1. PowerShell syntax of every script in release/ and tools/.
  2. stage.iss (static, a cheap stand-in when ISCC is not installed): UTF-8
     BOM, every custom message in English and Japanese, every Check: function
     defined, begin/end blocks balanced, the finish page names the real
     Tools menu item. THIRD_PARTY_NOTICES.md links every licenses/ folder.
  3. OBS detection and layout rules (obs-detect.ps1 mirrors stage.iss): unit
     cases, constants in sync with stage.iss, buildspec.json and
     tools/fetch-obs-headers.ps1, and the real OBS on this machine (read-only,
     skip with -SkipLocalObs).
  4. Protocol and preset contracts (node tests/contracts/*.mjs) when Node.js
     is on PATH.
  5. actionlint on .github/workflows (go run, pinned version; needs Go).

  Windows PowerShell 5.1 compatible. Exit code = number of failed checks.

.EXAMPLE
  powershell -NoProfile -File release/check.ps1
  powershell -NoProfile -File release/check.ps1 -SkipActionlint -SkipLocalObs
#>
[CmdletBinding()]
param(
    [switch]$SkipActionlint,
    [switch]$SkipLocalObs
)
Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$ActionlintVersion = 'v1.7.12'
$script:Failures = 0

function Pass([string]$m) { Write-Host "  ok    $m" -ForegroundColor Green }
function Fail([string]$m) { Write-Host "  FAIL  $m" -ForegroundColor Red; $script:Failures++ }
function Section([string]$m) { Write-Host "== $m" -ForegroundColor Cyan }
function Expect($got, $want, [string]$what) {
    if ("$got" -eq "$want") { Pass "$what = $want" } else { Fail "$what = '$got', want '$want'" }
}

# ---- 1. PowerShell syntax ----------------------------------------------------
Section 'PowerShell syntax'
foreach ($f in @(Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*.ps1') + @(Get-ChildItem -LiteralPath (Join-Path $Repo 'tools') -Filter '*.ps1')) {
    $tokens = $null; $errors = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile($f.FullName, [ref]$tokens, [ref]$errors)
    if ($errors) { Fail "$($f.Name): $($errors[0].Message) (line $($errors[0].Extent.StartLineNumber))" } else { Pass $f.Name }
}

# ---- 2. stage.iss ----------------------------------------------------------------
# Pascal source with string literals, { } / (* *) / // comments and ISPP
# lines (#...) blanked out.
function Remove-PascalNoise([string]$Src) {
    $sb = New-Object Text.StringBuilder
    $i = 0; $n = $Src.Length
    while ($i -lt $n) {
        $c = $Src[$i]
        if ($c -eq "'") {
            $j = $Src.IndexOf("'", $i + 1); if ($j -lt 0) { break }
            [void]$sb.Append("''"); $i = $j + 1
        } elseif ($c -eq '{') {
            $j = $Src.IndexOf('}', $i + 1); if ($j -lt 0) { break }
            [void]$sb.Append(' '); $i = $j + 1
        } elseif ($c -eq '(' -and $i + 1 -lt $n -and $Src[$i + 1] -eq '*') {
            $j = $Src.IndexOf('*)', $i + 2); if ($j -lt 0) { break }
            [void]$sb.Append(' '); $i = $j + 2
        } elseif ($c -eq '/' -and $i + 1 -lt $n -and $Src[$i + 1] -eq '/') {
            $j = $Src.IndexOf("`n", $i); if ($j -lt 0) { $j = $n }
            $i = $j
        } elseif ($c -eq '#' -and ($i -eq 0 -or $Src[$i - 1] -eq "`n")) {
            $j = $Src.IndexOf("`n", $i); if ($j -lt 0) { $j = $n }
            $i = $j
        } else {
            [void]$sb.Append($c); $i++
        }
    }
    return $sb.ToString()
}

Section 'Inno Setup script (static)'
$name = 'stage.iss'
$common = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'inno\common.iss') -Raw -Encoding UTF8
$text = Get-Content -LiteralPath (Join-Path $PSScriptRoot $name) -Raw -Encoding UTF8
$bytes = [IO.File]::ReadAllBytes((Join-Path $PSScriptRoot $name))
if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) { Pass "$name has a UTF-8 BOM" }
else { Fail "$name must be saved as UTF-8 with BOM (Japanese messages)" }
# ISCC reads any line starting with "[" as a section tag, also inside [Code].
foreach ($f in @(@($name, $text), @('inno\common.iss', $common))) {
    $code = [regex]::Split($f[1], '(?m)^\[Code\]\s*$')
    $bad = @(if ($code.Count -gt 1) { [regex]::Matches($code[1], '(?m)^[ \t]*\[.*$') | ForEach-Object { $_.Value.Trim() } })
    if ($bad) { Fail "$($f[0]): [Code] line starts with '[' (ISCC: Invalid section tag): $($bad -join ' | ')" }
    else { Pass "$($f[0]): no [Code] line starts with '['" }
}
$used =@([regex]::Matches($text, "CustomMessage\('(\w+)'\)") | ForEach-Object { $_.Groups[1].Value }) +
        @([regex]::Matches($text, '\{cm:(\w+)') | ForEach-Object { $_.Groups[1].Value })
$builtin = @('UninstallProgram', 'CreateDesktopIcon', 'LaunchProgram')
foreach ($lang in 'english', 'japanese') {
    $defined = @([regex]::Matches($text, "(?m)^$lang\.(\w+)=") | ForEach-Object { $_.Groups[1].Value })
    $missing = @($used | Sort-Object -Unique | Where-Object { $_ -notin $builtin -and $_ -notin $defined })
    if ($missing) { Fail "$name ${lang}: missing messages $($missing -join ', ')" } else { Pass "$name ${lang}: $(@($used | Sort-Object -Unique).Count) messages defined" }
}
# The finish page names the real Tools menu item (data/locale).
foreach ($pair in @(@('english', 'en-US'), @('japanese', 'ja-JP'))) {
    $ini = Get-Content -LiteralPath (Join-Path $Repo "data\locale\$($pair[1]).ini") -Raw -Encoding UTF8
    $menu = [regex]::Match($ini, '(?m)^Stage\.Tools\.SetupScene="([^"]+)"').Groups[1].Value
    $label = [regex]::Match($text, "(?m)^$($pair[0])\.FinishedLabel=(.*)$").Groups[1].Value
    if ($menu -and $label.Contains($menu)) { Pass "$name $($pair[0]) FinishedLabel names the menu item '$menu'" }
    else { Fail "$name $($pair[0]) FinishedLabel does not name the Tools menu item '$menu' ($($pair[1]).ini Stage.Tools.SetupScene)" }
}

Section 'third-party notices'
# Every licence text THIRD_PARTY_NOTICES.md links exists, and every folder in
# licenses/ is linked (package.ps1 ships both).
$notices = Get-Content -LiteralPath (Join-Path $Repo 'THIRD_PARTY_NOTICES.md') -Raw -Encoding UTF8
$links = @([regex]::Matches($notices, '\]\((licenses/[^)]+)\)') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
foreach ($l in $links) {
    if (Test-Path -LiteralPath (Join-Path $Repo $l) -PathType Leaf) { Pass "notices link $l" } else { Fail "THIRD_PARTY_NOTICES.md links missing file $l" }
}
foreach ($d in @(Get-ChildItem -LiteralPath (Join-Path $Repo 'licenses') -Directory)) {
    if (@($links | Where-Object { $_.StartsWith("licenses/$($d.Name)/") }).Count -gt 0) { Pass "licenses/$($d.Name) is listed" }
    else { Fail "licenses/$($d.Name) is not listed in THIRD_PARTY_NOTICES.md" }
}
$codeAt = [regex]::Match($text, '(?m)^\[Code\]').Index
$commonAt = [regex]::Match($common, '(?m)^\[Code\]').Index
$sections = $text.Substring(0, $codeAt)
$code = $text.Substring($codeAt) + "`n" + $common.Substring($commonAt)
foreach ($c in @([regex]::Matches($sections, 'Check:\s*(?:not\s+)?(\w+)') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)) {
    if ($code -match "(?mi)^function\s+$c\s*\(") { Pass "$name Check: $c defined" } else { Fail "$name Check: $c has no function" }
}
$plain = Remove-PascalNoise $code
$opens = [regex]::Matches($plain, '(?i)\b(begin|case|try)\b').Count
$ends = [regex]::Matches($plain, '(?i)\bend\b').Count
if ($opens -eq $ends) { Pass "$name begin/end balanced ($opens)" } else { Fail "$name begin/case/try=$opens, end=$ends" }

# ---- 3. OBS detection / layout -------------------------------------------------
Section 'OBS layout rules'
. (Join-Path $PSScriptRoot 'obs-detect.ps1')
Expect (Get-StageLayout '32.2.2.0') 'legacy' 'layout(OBS 32.2.2)'
Expect (Get-StageLayout '32.0.0.0') 'legacy' 'layout(OBS 32.0.0)'
Expect (Get-StageLayout '33.0.0.0') 'flat' 'layout(OBS 33.0.0)'
Expect (Get-StageLayout '34.1.2.0') 'flat' 'layout(OBS 34.1.2)'
Expect (Get-StageLayout '31.1.1.0') 'unsupported' 'layout(OBS 31.1.1)'
Expect (Get-StageLayout $null) 'both' 'layout(unknown)'
Expect (Get-StageLayout '32.2.2.0' 'flat') 'flat' 'layout(32.2.2, /LAYOUT=flat)'
Expect ((Get-StageDllPaths 'legacy') -join ';') 'bin\64bit\parallaxa.dll' 'dll paths(legacy)'
Expect ((Get-StageDllPaths 'legacy' -AlsoFlat) -join ';') 'parallaxa.dll;bin\64bit\parallaxa.dll' 'dll paths(legacy + future task)'
Expect ((Get-StageDllPaths 'flat') -join ';') 'parallaxa.dll' 'dll paths(flat)'
Expect ((Get-StageDefaultDllPaths '32.2.2.0') -join ';') 'parallaxa.dll;bin\64bit\parallaxa.dll' 'default install on OBS 32.2.2 writes both layouts'
Expect ((Get-StageDefaultDllPaths '33.0.0.0') -join ';') 'parallaxa.dll' 'default install on OBS 33.0.0 writes the flat layout only'
$vdf = @('"libraryfolders"', '{', '	"0"', '	{', '		"path"		"C:\\Program Files (x86)\\Steam"', '	}', '	"1"', '	{', '		"path"		"D:\\SteamLibrary"', '		"label"		""', '	}', '}')
Expect ((Get-SteamLibraryPaths $vdf) -join ';') 'C:\Program Files (x86)\Steam;D:\SteamLibrary' 'libraryfolders.vdf parsing'

$spec = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'buildspec.json') -Raw | ConvertFrom-Json
Expect ([regex]::Match($text, '#define MinObsMajor (\d+)').Groups[1].Value) $script:StageMinObsMajor 'stage.iss MinObsMajor'
Expect ([regex]::Match($text, '#define FlatFromMajor (\d+)').Groups[1].Value) $script:StageFlatFromMajor 'stage.iss FlatFromMajor'
Expect (($spec.supportedObs.min -split '\.')[0]) $script:StageMinObsMajor 'buildspec supportedObs.min major'
Expect (($spec.supportedObs.flatLayoutFrom -split '\.')[0]) $script:StageFlatFromMajor 'buildspec flatLayoutFrom major'
$hdr = Get-Content -LiteralPath (Join-Path $Repo 'tools\fetch-obs-headers.ps1') -Raw
Expect ([regex]::Match($hdr, "\`$Tag\s*=\s*'([^']+)'").Groups[1].Value) $spec.dependencies.'obs-studio'.version 'header tag (fetch-obs-headers default) = buildspec'
$smoke = @($spec.obsSmoke)
$bin = $spec.dependencies.'obs-binaries'
$same = @($smoke | Where-Object { $_.version -eq $bin.version -and $_.sha256 -eq $bin.sha256 -and $_.url -eq $bin.url })
Expect $same.Count 1 "obsSmoke has the build target OBS $($bin.version) (same zip as obs-binaries)"
Expect (@($smoke | Where-Object { $_.version -like "$(($spec.supportedObs.flatLayoutFrom -split '\.')[0]).*" -and $_.sha256 -match '^[0-9a-f]{64}$' }).Count -ge 1) $true "obsSmoke has an OBS $($spec.supportedObs.flatLayoutFrom -replace '\..*$', '').x build (flat layout)"
$cmakeVersion = [regex]::Match((Get-Content -LiteralPath (Join-Path $Repo 'CMakeLists.txt') -Raw), 'project\([^)]*VERSION\s+([0-9.]+)').Groups[1].Value
$dockVersion = (Get-Content -LiteralPath (Join-Path $Repo 'dock-src\package.json') -Raw | ConvertFrom-Json).version
Expect $dockVersion $cmakeVersion 'dock-src/package.json version = CMakeLists.txt project(VERSION)'

if (-not $SkipLocalObs) {
    $obs = Find-ObsDir
    if ($obs) {
        $v = Get-ObsFileVersion $obs
        $layout = Get-StageLayout $v
        Pass "this machine: OBS $v at $obs -> $layout layout; default install writes $((Get-StageDefaultDllPaths $v) -join ', ')"
        if ($v -like '32.*') { Expect $layout 'legacy' 'installed OBS 32.x picks bin\64bit' }
        if ([int](($v -split '\.')[0]) -ge 33) { Expect $layout 'flat' 'installed OBS 33+ picks the flat layout' }
    } else {
        Write-Host '  skip  no OBS found on this machine'
    }
}

# ---- 4. contracts ----------------------------------------------------------------
Section 'protocol and preset contracts'
if (Get-Command node -ErrorAction SilentlyContinue) {
    foreach ($t in @('protocol_check.mjs', 'presets_check.mjs')) {
        & node (Join-Path $Repo "tests\contracts\$t")
        if ($LASTEXITCODE -eq 0) { Pass $t } else { Fail $t }
    }
} else {
    Write-Host '  skip  node not on PATH'
}

# ---- 5. actionlint -------------------------------------------------------------------
if (-not $SkipActionlint) {
    Section "actionlint $ActionlintVersion"
    if (Get-Command go -ErrorAction SilentlyContinue) {
        Push-Location $Repo
        try {
            $env:CGO_ENABLED = '0'
            & go run "github.com/rhysd/actionlint/cmd/actionlint@$ActionlintVersion" -color
            if ($LASTEXITCODE -eq 0) { Pass 'actionlint: .github/workflows clean' } else { Fail 'actionlint' }
        } finally { Pop-Location }
    } else {
        Write-Host '  skip  go not on PATH (actionlint runs with go run)'
    }
}

Write-Host ''
if ($script:Failures -eq 0) { Write-Host 'check: all passed' -ForegroundColor Green } else { Write-Host "check: $($script:Failures) failed" -ForegroundColor Red }
exit $script:Failures
