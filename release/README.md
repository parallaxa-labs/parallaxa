# Release engineering (Parallaxa)

All scripts run on Windows PowerShell 5.1 (`powershell -NoProfile -File ...`) and on PowerShell 7.

| File | Purpose |
|---|---|
| `package.ps1` | Build + tests, stage `dist/release/stage/parallaxa` (both OBS layouts), plugin zip, PDB zip, GPL source zip, installer (Inno Setup 6.3+), `SHA256SUMS.txt` |
| `stage.iss`, `inno/common.iss` | Installer: admin, `%ProgramData%\obs-studio\plugins\parallaxa`, layout by OBS version |
| `obs-detect.ps1` | The installer's OBS detection and layout rules in PowerShell (tests, diagnostics) |
| `buildspec.json` | obs-plugintemplate-style pins: OBS 32.0.0 headers and binaries (SHA-256), SDK 10.0.26100, `obsSmoke` builds (32.0.0, 33.0.0) |
| `obs-smoke.ps1` | Loads `parallaxa.dll` into the pinned official OBS builds: import check + headless libobs load |
| `sign.ps1` | Authenticode signing when a certificate is configured (`SIGN_PFX_BASE64` + `SIGN_PFX_PASSWORD`, or `SIGN_PFX_PATH`), otherwise a no-op |
| `check.ps1` | Release lint: script syntax, installer static checks, layout rules, buildspec consistency, contract checks, actionlint |

```powershell
powershell -NoProfile -File release/check.ps1
powershell -NoProfile -File release/package.ps1 -AllowDirtySource          # development build
powershell -NoProfile -File release/package.ps1 -Ci -RequireInstaller       # what CI runs (downloads ~400 MB of OBS zips once)
```

Outputs in `dist/release/`: `Parallaxa-<v>-windows-x64.zip` (both layouts, manual install),
`-pdb.zip`, `-source.zip`, `installers/Parallaxa-<v>-Setup.exe`, `SHA256SUMS.txt`.

## GPL source

`Parallaxa-<v>-source.zip` is this repository at the released commit (`git archive HEAD`),
which is the complete corresponding source: the plugin, its build and release scripts, the
installer script, `docs/protocol` and `dock-src/` (source and build configuration of the minified
`data/dock/dock-standalone.html`). `package.ps1` refuses to package a tree with uncommitted changes
unless `-AllowDirtySource` (then the zip holds the working tree and `SOURCE.txt` says so).
Publish binaries and the source zip together on the release page; `SOURCE.txt` in every package
points there.

## Manual acceptance (a person runs these; they write to `C:\ProgramData` and start OBS)

1. Close OBS. Run `Parallaxa-<v>-Setup.exe /LOG=%TEMP%\parallaxa-setup.log`. On OBS 32.x the OBS page
   names the bin\64bit layout, the "Also install the copy for OBS 33 and later" task is ticked, and
   afterwards both `...\plugins\parallaxa\bin\64bit\parallaxa.dll` and `...\plugins\parallaxa\parallaxa.dll`
   exist (plus `data\`).
2. Start OBS: the log shows `[parallaxa] loaded`. Close OBS, uninstall: `...\plugins\parallaxa` is gone.
3. With OBS 33 (or `/OBSDIR=` pointing at an OBS 33 folder): only the flat `parallaxa.dll` is installed.
4. Dev install and leak check: `tools/install-dev.ps1`, add the three filters, run a preset, close OBS;
   the log ends with `Number of memory leaks: 0`. `tools/install-dev.ps1 -Uninstall` afterwards.
