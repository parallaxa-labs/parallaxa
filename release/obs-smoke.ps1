<#
.SYNOPSIS
  Smoke-tests parallaxa.dll against real OBS builds (default: every
  entry of buildspec.json "obsSmoke", i.e. OBS 32.0.0 and 33.0.0).

.DESCRIPTION
  The plugin is built against the OBS 32.0.0 headers and import libraries,
  but Steam updates OBS silently, so it must also load on 33.x. For each
  pinned official OBS Windows zip (SHA-256 checked, cached in dist/cache):
    1. Import check: every function parallaxa.dll imports from an OBS
       DLL (obs.dll, obs-frontend-api.dll, w32-pthreads.dll, ...) must be
       exported by that OBS build's DLL; every other imported DLL must be part
       of Windows (System32 or an API set). Uses dumpbin.
    2. Headless load (skip with -NoLoad), in a child process: libobs
       obs_startup() without video, obs_open_module() + obs_init_module() on
       the DLL (this runs the version check that refuses modules built
       against newer headers, and obs_module_load), checks that the filters
       named in include/stage-protocol.h are registered, then
       obs_shutdown() (runs obs_module_unload). No window, no OBS UI, no
       profile: nothing is written outside dist/.
  Runs on any Windows x64 machine with Visual Studio's C++ tools (dumpbin).
  Windows PowerShell 5.1 compatible.

.EXAMPLE
  powershell -NoProfile -File release/obs-smoke.ps1 -PluginDir dist/release/stage/parallaxa
  powershell -NoProfile -File release/obs-smoke.ps1 -PluginDir build -ObsVersion 33.0.0
#>
[CmdletBinding()]
param(
    [string]$PluginDir,         # folder with parallaxa.dll (and data\, else the repo's data\ is used)
    [string[]]$ObsVersion,      # default: every buildspec obsSmoke entry
    [string]$Cache,
    [switch]$NoLoad,
    # internal: the headless load runs in a child process
    [switch]$LoadChild, [string]$ObsBin, [string]$Dll, [string]$DataDir, [string[]]$Filters
)
Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# ---------------------------------------------------------------------------
# Child: headless libobs load. Exit 0 = loaded, filters registered.
# ---------------------------------------------------------------------------
if ($LoadChild) {
    $Filters = @($Filters | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class StageObsSmoke {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool SetDllDirectoryW(string path);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr AddDllDirectory(string path);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr LoadLibraryW(string path);
    const string L = "obs.dll";
    [DllImport(L, CallingConvention = CallingConvention.Cdecl)] [return: MarshalAs(UnmanagedType.I1)]
    public static extern bool obs_startup(byte[] locale, byte[] moduleConfigPath, IntPtr store);
    [DllImport(L, CallingConvention = CallingConvention.Cdecl)]
    public static extern int obs_open_module(out IntPtr module, byte[] path, byte[] dataPath);
    [DllImport(L, CallingConvention = CallingConvention.Cdecl)] [return: MarshalAs(UnmanagedType.I1)]
    public static extern bool obs_init_module(IntPtr module);
    [DllImport(L, CallingConvention = CallingConvention.Cdecl)] [return: MarshalAs(UnmanagedType.I1)]
    public static extern bool obs_enum_filter_types(UIntPtr idx, out IntPtr id);
    [DllImport(L, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr obs_get_version_string();
    [DllImport(L, CallingConvention = CallingConvention.Cdecl)]
    public static extern void obs_shutdown();
    public static byte[] U(string s) { return s == null ? null : Encoding.UTF8.GetBytes(s + "\0"); }
    public static string S(IntPtr p) {
        if (p == IntPtr.Zero) return null;
        int n = 0; while (Marshal.ReadByte(p, n) != 0) n++;
        byte[] b = new byte[n]; Marshal.Copy(p, b, 0, n);
        return Encoding.UTF8.GetString(b);
    }
}
'@
    # obs.dll and its dependencies (ffmpeg, zlib, ...) come from this OBS build
    # only (SetDllDirectory/AddDllDirectory serve the DllImports below). In real
    # OBS, obs64.exe imports obs-frontend-api.dll, so it is already loaded when
    # a plugin loads; libobs's os_dlopen does not search the OBS folder for it
    # (32.x switches the DLL directory to the plugin's own folder), so load it
    # up front like obs64.exe does.
    if (-not [StageObsSmoke]::SetDllDirectoryW($ObsBin)) { throw "SetDllDirectory($ObsBin) failed" }
    if ([StageObsSmoke]::AddDllDirectory($ObsBin) -eq [IntPtr]::Zero) { throw "AddDllDirectory($ObsBin) failed" }
    foreach ($pre in @('obs.dll', 'obs-frontend-api.dll')) {
        if ([StageObsSmoke]::LoadLibraryW((Join-Path $ObsBin $pre)) -eq [IntPtr]::Zero) { throw "LoadLibrary($pre) failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())" }
    }
    Set-Location -LiteralPath $ObsBin
    if (-not [StageObsSmoke]::obs_startup([StageObsSmoke]::U('en-US'), $null, [IntPtr]::Zero)) { throw 'obs_startup failed' }
    $ver = [StageObsSmoke]::S([StageObsSmoke]::obs_get_version_string())
    Write-Host "    libobs $ver started (no video)"
    $mod = [IntPtr]::Zero
    $code = [StageObsSmoke]::obs_open_module([ref]$mod, [StageObsSmoke]::U($Dll), [StageObsSmoke]::U($DataDir))
    # MODULE_SUCCESS 0, ERROR -1, FILE_NOT_FOUND -2, MISSING_EXPORTS -3, INCOMPATIBLE_VER -4, HARDCODED_SKIP -5
    $names = @{ '0' = 'success'; '-1' = 'error'; '-2' = 'file not found / imports unresolved'; '-3' = 'missing exports'; '-4' = 'incompatible libobs version'; '-5' = 'skipped' }
    if ($code -ne 0) { throw "obs_open_module returned $code ($($names["$code"]))" }
    if (-not [StageObsSmoke]::obs_init_module($mod)) { throw 'obs_init_module failed (obs_module_load returned false)' }
    $have = @{}
    $i = 0
    $id = [IntPtr]::Zero
    while ([StageObsSmoke]::obs_enum_filter_types([UIntPtr][uint64]$i, [ref]$id)) { $have[[StageObsSmoke]::S($id)] = $true; $i++ }
    $missing = @($Filters | Where-Object { -not $have.ContainsKey($_) })
    if ($missing) { throw "filters not registered: $($missing -join ', ')" }
    Write-Host "    module loaded; filters registered: $($Filters -join ', ')"
    [StageObsSmoke]::obs_shutdown()
    Write-Host '    obs_shutdown ok (module unloaded)'
    exit 0
}

# ---------------------------------------------------------------------------
# Parent
# ---------------------------------------------------------------------------
$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Spec = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'buildspec.json') -Raw | ConvertFrom-Json
if (-not $Cache) { $Cache = Join-Path $Repo 'dist\cache' }
if (-not $PluginDir) { $PluginDir = Join-Path $Repo 'dist\release\stage\parallaxa' }
$PluginDir = (Resolve-Path -LiteralPath $PluginDir).Path
$DllPath = Join-Path $PluginDir 'parallaxa.dll'
if (-not (Test-Path -LiteralPath $DllPath)) { throw "$DllPath not found" }
$Data = Join-Path $PluginDir 'data'
if (-not (Test-Path -LiteralPath $Data)) { $Data = Join-Path $Repo 'data' }
$tar = Join-Path $env:SystemRoot 'System32\tar.exe'
$curl = Join-Path $env:SystemRoot 'System32\curl.exe'

$proto = Get-Content -LiteralPath (Join-Path $Repo 'include\stage-protocol.h') -Raw
$FilterIds = @([regex]::Matches($proto, '#define\s+STAGE_FILTER_\w+\s+"([a-z0-9_]+)"') | ForEach-Object { $_.Groups[1].Value })
if (-not $FilterIds) { throw 'no STAGE_FILTER_* ids found in stage-protocol.h' }

function Find-Dumpbin {
    $c = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $p = & $vswhere -latest -products * -find 'VC\Tools\MSVC\**\bin\Hostx64\x64\dumpbin.exe' | Select-Object -First 1
        if ($p) { return $p }
    }
    throw 'dumpbin.exe not found (Visual Studio C++ tools)'
}
$Dumpbin = Find-Dumpbin

# Downloads (once, SHA-256 checked) and extracts bin\64bit\*.dll of one OBS build.
function Get-ObsBin($Entry) {
    $dir = Join-Path $Cache "obs-$($Entry.version)-smoke"
    $bin = Join-Path $dir 'bin\64bit'
    if (Test-Path -LiteralPath (Join-Path $dir '.complete')) { return $bin }
    New-Item -ItemType Directory -Force -Path $Cache | Out-Null
    $zip = Join-Path $Cache (Split-Path -Leaf $Entry.url)
    $ok = (Test-Path -LiteralPath $zip) -and ((Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash.ToLowerInvariant() -eq $Entry.sha256)
    if (-not $ok) {
        Write-Host "    download $($Entry.url)"
        & $curl --fail --location --silent --show-error --retry 3 --output "$zip.part" $Entry.url
        if ($LASTEXITCODE -ne 0) { throw "download failed ($LASTEXITCODE)" }
        $h = (Get-FileHash -Algorithm SHA256 -LiteralPath "$zip.part").Hash.ToLowerInvariant()
        if ($h -ne $Entry.sha256) { Remove-Item -LiteralPath "$zip.part" -Force; throw "SHA-256 mismatch for $($Entry.url): $h" }
        Move-Item -LiteralPath "$zip.part" -Destination $zip -Force
    }
    if (Test-Path -LiteralPath $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $members = @(& $tar -tf $zip | Where-Object { $_ -match '^bin/64bit/[^/]+\.dll$' })
    if ($LASTEXITCODE -ne 0 -or -not $members) { throw "cannot list $zip" }
    & $tar -xf $zip -C $dir @members
    if ($LASTEXITCODE -ne 0) { throw "tar failed ($LASTEXITCODE)" }
    Set-Content -LiteralPath (Join-Path $dir '.complete') -Value $Entry.sha256 -Encoding ascii
    return $bin
}

# dumpbin /IMPORTS -> @{ 'obs.dll' = @('obs_source_get_name', ...) } (keys lower-case).
function Get-Imports([string]$Path) {
    $map = @{}
    $cur = $null
    foreach ($line in (& $Dumpbin /NOLOGO /IMPORTS $Path)) {
        if ($line -match '^\s{4}(\S+\.dll)\s*$') { $cur = $Matches[1].ToLowerInvariant(); $map[$cur] = New-Object System.Collections.Generic.List[string]; continue }
        if (-not $cur) { continue }
        if ($line -match '^\s+Ordinal\s+(\d+)\s*$') { $map[$cur].Add("#$($Matches[1])"); continue }
        if ($line -match '^\s+[0-9A-Fa-f]+\s+(\S+)\s*$') { $map[$cur].Add($Matches[1]) }
        if ($line -match '^\s+Summary\s*$') { break }
    }
    return $map
}

# dumpbin /EXPORTS -> set of exported names.
function Get-Exports([string]$Path) {
    $set = New-Object 'System.Collections.Generic.HashSet[string]'
    foreach ($line in (& $Dumpbin /NOLOGO /EXPORTS $Path)) {
        if ($line -match '^\s+\d+\s+[0-9A-Fa-f]+\s+(?:[0-9A-Fa-f]{8}\s+)?([A-Za-z_]\w*)') { [void]$set.Add($Matches[1]) }
    }
    return $set
}

$entries = @($Spec.obsSmoke)
if ($ObsVersion) { $entries = @($entries | Where-Object { $ObsVersion -contains $_.version }) }
if (-not $entries) { throw "no obsSmoke entry in buildspec.json for: $($ObsVersion -join ', ')" }

$imports = Get-Imports $DllPath
if (-not $imports.ContainsKey('obs.dll')) { throw "dumpbin found no obs.dll imports in $DllPath" }
$sys = Join-Path $env:SystemRoot 'System32'
$PowerShellExe = (Get-Process -Id $PID).Path
$failures = 0

foreach ($e in $entries) {
    Write-Host "==> OBS $($e.version)" -ForegroundColor Cyan
    $bin = Get-ObsBin $e
    $bad = @()
    $count = 0
    foreach ($dll in $imports.Keys) {
        $obsDll = Join-Path $bin $dll
        if (Test-Path -LiteralPath $obsDll) {
            $exp = Get-Exports $obsDll
            foreach ($fn in $imports[$dll]) {
                $count++
                if ($fn.StartsWith('#') -or -not $exp.Contains($fn)) { $bad += "$dll!$fn" }
            }
        } elseif (-not ($dll.StartsWith('api-ms-win-') -or $dll.StartsWith('ext-ms-') -or (Test-Path -LiteralPath (Join-Path $sys $dll)))) {
            $bad += "$dll (not in OBS $($e.version) bin\64bit, not in System32)"
        }
    }
    if ($bad) {
        Write-Host "    FAIL imports missing in OBS $($e.version):`n      $($bad -join "`n      ")" -ForegroundColor Red
        $failures++
        continue
    }
    Write-Host "    imports ok: $count OBS functions from $((@($imports.Keys | Where-Object { Test-Path -LiteralPath (Join-Path $bin $_) })) -join ', ')"

    if ($NoLoad) { continue }
    $a = @('-NoProfile', '-File', $PSCommandPath, '-LoadChild', '-ObsBin', $bin, '-Dll', $DllPath, '-DataDir', $Data, '-Filters', ($FilterIds -join ','))
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue' # the child's stderr is data here
    try { $out = @(& $PowerShellExe @a 2>&1 | ForEach-Object { "$_" }); $code = $LASTEXITCODE } finally { $ErrorActionPreference = $old }
    $out | Where-Object { ($_ -match '^\s{4}|\[stage\]|error') -and ($_ -notmatch 'Tried to call obs_frontend_\w+ with no callbacks') } | ForEach-Object { Write-Host "    $($_.TrimStart())" }
    if ($code -ne 0) {
        Write-Host "    FAIL headless load on OBS $($e.version) (exit $code)" -ForegroundColor Red
        $out | Select-Object -Last 20 | ForEach-Object { Write-Host "      $_" }
        $failures++
    } else {
        Write-Host "    headless load ok on OBS $($e.version)" -ForegroundColor Green
    }
}
if ($failures) { throw "OBS smoke test failed for $failures OBS build(s)" }
Write-Host '==> OBS smoke test passed' -ForegroundColor Green
