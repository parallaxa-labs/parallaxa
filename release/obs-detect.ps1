# OBS detection and plugin-layout choice, the same rules as the [Code] section
# of release/stage.iss (keep them in sync). Dot-source it:
#   . release/obs-detect.ps1
#   $dir = Find-ObsDir; Get-StageLayout (Get-ObsFileVersion $dir)
# Read-only: it only reads the registry, libraryfolders.vdf and obs64.exe's
# version resource. Windows PowerShell 5.1 compatible.

$script:StageMinObsMajor = 32     # headers are pinned to 32.0.0
$script:StageFlatFromMajor = 33   # OBS 33 introduced the flat plugin layout

function Test-ObsDir([string]$Dir) {
    return [bool]($Dir -and (Test-Path -LiteralPath (Join-Path $Dir 'bin\64bit\obs64.exe')))
}

# obs64.exe's fixed file version as "a.b.c.d" (what Inno's
# GetVersionNumbersString returns), or $null.
function Get-ObsFileVersion([string]$Dir) {
    if (-not (Test-ObsDir $Dir)) { return $null }
    $vi = (Get-Item -LiteralPath (Join-Path $Dir 'bin\64bit\obs64.exe')).VersionInfo
    if ($vi.FileMajorPart -le 0) { return $null }
    return '{0}.{1}.{2}.{3}' -f $vi.FileMajorPart, $vi.FileMinorPart, $vi.FileBuildPart, $vi.FilePrivatePart
}

# Library paths from Steam's libraryfolders.vdf text ("path"  "D:\\SteamLibrary").
function Get-SteamLibraryPaths([string[]]$VdfLines) {
    foreach ($raw in $VdfLines) {
        $line = $raw.Trim()
        if ($line.ToLowerInvariant().StartsWith('"path"')) {
            $lib = $line.Substring(6).Trim()
            if ($lib.Length -ge 2 -and $lib.StartsWith('"') -and $lib.EndsWith('"')) { $lib = $lib.Substring(1, $lib.Length - 2) }
            $lib.Replace('\\', '\')
        }
    }
}

function Get-RegString([string]$Path, [string]$Name) {
    try { return (Get-ItemProperty -LiteralPath $Path -Name $Name -ErrorAction Stop).$Name } catch { return $null }
}

function Find-SteamObsDir {
    $steam = Get-RegString 'HKCU:\Software\Valve\Steam' 'SteamPath'
    if (-not $steam) { $steam = Get-RegString 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam' 'InstallPath' }
    if (-not $steam) { return $null }
    $steam = $steam.Replace('/', '\')
    $d = Join-Path $steam 'steamapps\common\OBS Studio'
    if (Test-ObsDir $d) { return $d }
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (-not (Test-Path -LiteralPath $vdf)) { return $null }
    foreach ($lib in (Get-SteamLibraryPaths (Get-Content -LiteralPath $vdf))) {
        $d = Join-Path $lib 'steamapps\common\OBS Studio'
        if (Test-ObsDir $d) { return $d }
    }
    return $null
}

# Same order as DetectObsDir() in stage.iss.
function Find-ObsDir {
    $d = Get-RegString 'HKLM:\SOFTWARE\OBS Studio' '(default)'
    if (Test-ObsDir $d) { return $d }
    $d = Get-RegString 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio' 'InstallLocation'
    if (Test-ObsDir $d) { return $d }
    $d = Find-SteamObsDir
    if ($d) { return $d }
    $d = Join-Path $env:ProgramFiles 'obs-studio'
    if (Test-ObsDir $d) { return $d }
    return $null
}

# 'flat' (33+), 'legacy' (32.x), 'both' (unknown) or 'unsupported' (< 32),
# like ChosenLayout() + the version check in stage.iss.
function Get-StageLayout([string]$FileVersion, [string]$Forced = '') {
    switch ($Forced.ToLowerInvariant()) {
        'flat' { return 'flat' }
        'legacy' { return 'legacy' }
        'both' { return 'both' }
    }
    if (-not $FileVersion) { return 'both' }
    $major = 0
    if (-not [int]::TryParse(($FileVersion -split '\.')[0], [ref]$major) -or $major -le 0) { return 'both' }
    if ($major -lt $script:StageMinObsMajor) { return 'unsupported' }
    if ($major -ge $script:StageFlatFromMajor) { return 'flat' }
    return 'legacy'
}

# Where the DLL goes for a layout, relative to ...\plugins\parallaxa.
# -AlsoFlat = stage.iss's "future" task, checked by default on OBS 32, so a
# default install on 32.x writes both copies (see Get-StageDefaultDllPaths).
function Get-StageDllPaths([string]$Layout, [switch]$AlsoFlat) {
    switch ($Layout) {
        'flat' { return @('parallaxa.dll') }
        'legacy' { if ($AlsoFlat) { return @('parallaxa.dll', 'bin\64bit\parallaxa.dll') } else { return @('bin\64bit\parallaxa.dll') } }
        'both' { return @('parallaxa.dll', 'bin\64bit\parallaxa.dll') }
        default { return @() }
    }
}

# What a default install (no /LAYOUT=, "future" task left checked) writes.
function Get-StageDefaultDllPaths([string]$FileVersion) {
    $layout = Get-StageLayout $FileVersion
    return Get-StageDllPaths $layout -AlsoFlat:($layout -eq 'legacy')
}
