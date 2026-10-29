<#
.SYNOPSIS
  Authenticode-signs release files when a code-signing certificate is
  configured; otherwise leaves them unsigned (placeholder for CI secrets).

.DESCRIPTION
  Configuration comes from environment variables (GitHub Actions secrets in
  CI, never committed):
    SIGN_PFX_BASE64     base64 of the .pfx code-signing certificate, or
    SIGN_PFX_PATH       path of the .pfx file
    SIGN_PFX_PASSWORD   its password
    SIGN_TIMESTAMP_URL  RFC 3161 timestamp server (default http://timestamp.digicert.com)
  With none of them set the script prints "unsigned" and exits 0, so local
  and fork builds work. -Require makes a missing certificate an error
  (release tags).

  Also used as Inno Setup's SignTool for the installers and uninstallers:
    ISCC "/Sparallaxa=powershell -NoProfile -File <repo>\release\sign.ps1 $f" /DSignTool=parallaxa ...
  (package.ps1 passes this only when a certificate is configured).

  Windows PowerShell 5.1 compatible.

.EXAMPLE
  powershell -NoProfile -File release/sign.ps1 build\parallaxa.dll
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)]
    [string[]]$Files,
    [switch]$Require
)
Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

function Test-SigningConfigured {
    return [bool]($env:SIGN_PFX_BASE64 -or $env:SIGN_PFX_PATH)
}

if (-not (Test-SigningConfigured)) {
    if ($Require) { throw 'code signing required but SIGN_PFX_BASE64 / SIGN_PFX_PATH is not set' }
    foreach ($f in $Files) { Write-Host "unsigned (no certificate configured): $f" }
    exit 0
}

# signtool.exe from the newest Windows 10/11 SDK.
function Get-SignTool {
    $c = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    $found = Get-ChildItem -Path $kits -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^10\.' } | Sort-Object { [version]$_.Name } -Descending |
        ForEach-Object { Join-Path $_.FullName 'x64\signtool.exe' } | Where-Object { Test-Path $_ } |
        Select-Object -First 1
    if (-not $found) { throw 'signtool.exe not found (install the Windows SDK signing tools)' }
    return $found
}

$signtool = Get-SignTool
$ts = if ($env:SIGN_TIMESTAMP_URL) { $env:SIGN_TIMESTAMP_URL } else { 'http://timestamp.digicert.com' }
$pfx = $env:SIGN_PFX_PATH
$tmpPfx = $null
if (-not $pfx) {
    $tmpPfx = Join-Path ([IO.Path]::GetTempPath()) ("sign-" + [guid]::NewGuid().ToString('N') + '.pfx')
    [IO.File]::WriteAllBytes($tmpPfx, [Convert]::FromBase64String($env:SIGN_PFX_BASE64))
    $pfx = $tmpPfx
}
try {
    foreach ($f in $Files) {
        $signArgs = @('sign', '/fd', 'sha256', '/td', 'sha256', '/tr', $ts, '/f', $pfx)
        if ($env:SIGN_PFX_PASSWORD) { $signArgs += @('/p', $env:SIGN_PFX_PASSWORD) }
        $signArgs += $f
        & $signtool @signArgs | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "signtool failed ($LASTEXITCODE) on $f" }
        Write-Host "signed: $f"
    }
} finally {
    if ($tmpPfx) { Remove-Item -LiteralPath $tmpPfx -Force -ErrorAction SilentlyContinue }
}
