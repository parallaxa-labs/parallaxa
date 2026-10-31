# Parallaxa

**Parallaxa** is a free OBS Studio plugin for 2D VTubers by Parallaxa Labs: depth and parallax across your scene layers, a virtual camera (zoom, pan, rotate, shake, automatic drift, audio-reactive moves), single-pass colour looks and effects, per-layer depth blur, a control dock, and an obs-websocket vendor API (`parallaxa`) that other apps can drive. It works with your existing OBS scenes and any avatar source (VTube Studio through Spout2, a PNGTuber image, a webcam). Native C17, no Qt, zero extra render passes for the camera.

- **User guide:** [English](docs/user/en/guide.md) · [日本語](docs/user/ja/guide.md) · [Bahasa Indonesia](docs/user/id/guide.md)
- **Licence:** GPL-2.0-or-later: version 2 of the GNU GPL ([LICENSE](LICENSE)) or, at your option, any later version ([NOTICE](NOTICE)). The protocol docs in [docs/protocol](docs/protocol) are MIT. "Parallaxa" and "Parallaxa Labs" are trademarks of Parallaxa Labs; the licences do not grant trademark rights.
- **Requirements:** Windows 10/11 x64, OBS Studio 32.0 or later (33.x supported).

## Repository layout

```
CMakeLists.txt, cmake/   plugin build (MSVC + Ninja, against the OBS 32.0.0 headers)
include/stage-protocol.h shared protocol names, enums and limits (C mirror of docs/protocol)
src/                     core, camera rig, FX, depth blur, presets, obs-websocket vendor
data/                    effects (.effect), locale (en-US, ja-JP, id-ID), free presets, the dock
dock-src/                Svelte 5 source of data/dock/dock-standalone.html (GPL, see its README)
tests/                   unit, GPU, vendor and contract tests (ctest)
tools/                   fetch OBS headers, make import libraries, build, dev install, locale generator
release/                 package.ps1 (zips + installer), stage.iss, OBS smoke test, signing, release lint
docs/protocol/           vendor API, schemas and protocol data (MIT)
docs/ux/                 plugin UX specification (properties, locale keys)
docs/user/               user guide in three languages
```

## Build

Requirements: Visual Studio 2022 Build Tools with the C++ workload (bundled CMake and Ninja), PowerShell 5.1+, OBS Studio 32.x installed (for the import libraries), Node.js 22.12+ (contract tests and the dock) and pnpm 11 (dock only).

```powershell
powershell -NoProfile -File tools/fetch-obs-headers.ps1          # OBS 32.0.0 headers into third_party/obs-src
powershell -NoProfile -File tools/make-implibs.ps1               # import libraries from your OBS install
powershell -NoProfile -File tools/build.ps1                      # build/parallaxa.dll + every test (ctest)
powershell -NoProfile -File tools/install-dev.ps1                # optional: copy into %ProgramData% for OBS (admin)

pnpm -C dock-src install --frozen-lockfile; pnpm -C dock-src verify   # the standalone dock
powershell -NoProfile -File release/check.ps1                    # release lint
powershell -NoProfile -File release/package.ps1 -AllowDirtySource # dist/release: zip, PDB zip, source zip (+ installer with Inno Setup 6.3+)
```

`release/package.ps1 -Ci` is what CI runs: it takes the import libraries from the pinned official OBS 32.0.0 zip and smoke-loads the DLL into OBS 32.0.0 and 33.0.0 (`release/obs-smoke.ps1`). A release tag `v*` drafts a GitHub release with the plugin zip, the PDB zip, the installer and `Parallaxa-<version>-source.zip`, the complete corresponding source (this repository at that tag). `-SourceUrl` names the public repository in `SOURCE.txt`, the installer and the plugin's About row; it defaults to an `example.com` placeholder, which `-Ci` refuses, so the workflows pass the repository they run in.

## Protocol

Apps talk to Parallaxa through obs-websocket requests to the vendor `parallaxa` (`docs/protocol/vendor-api.md`, JSON Schemas in `docs/protocol/schemas`). The docs are MIT-licensed so that any app, free or commercial, can implement a client without touching GPL code. `tests/contracts/protocol_check.mjs` keeps `include/stage-protocol.h` identical to `docs/protocol/names.vendor.json` and `vendor-tables.json`; `tests/contracts/presets_check.mjs` validates every preset and writes `docs/protocol/builtin-presets.json`, the list of free presets clients can rely on.

## Supporting Parallaxa

Parallaxa is and stays free software: every feature of the plugin is in this repository, with no licence key, no time limit and no network access. Parallaxa Labs funds its development with two optional things:

- **Patreon membership.** Patrons support ongoing development (OBS updates, new camera moves and effects, fixes) and get early previews and a say in the roadmap. Join at [patreon.com/Parallaxa](https://www.patreon.com/Parallaxa).
- **Parallaxa Pro Pack.** A separate, paid content pack of extra looks: recipes, LUTs, camera moves and FX presets in the same JSON formats as the free presets in `data/presets` (`docs/protocol/vendor-api.md` section 6). It is a download, not code: you extract it into Parallaxa's user preset folder (`%APPDATA%\obs-studio\plugin_config\parallaxa\`) and the plugin loads it like any preset you saved yourself. Patrons of the Pro Pack tier receive it, and it is also sold as a one-time purchase on creator storefronts. The pack's content is under its own licence (personal and streaming use, no redistribution); it does not change the plugin's GPL licence.

Contributions of code, translations and presets are welcome; see [CONTRIBUTING.md](CONTRIBUTING.md).

---

## Ringkasan (Bahasa Indonesia)

**Parallaxa** adalah plugin OBS Studio gratis (GPL-2.0-or-later) dari Parallaxa Labs untuk VTuber 2D: kedalaman dan parallax antar lapisan, kamera virtual (zoom, pan, rotasi, getar, drift otomatis, reaktif audio), tampilan warna dan efek satu pass, blur kedalaman per lapisan, dock kendali, dan API vendor obs-websocket (`parallaxa`). Panduan: [docs/user/id/guide.md](docs/user/id/guide.md). Build: `tools/build.ps1` (MSVC), paket rilis: `release/package.ps1`. Dokumen protokol di `docs/protocol` berlisensi MIT. Pengembangan didukung lewat Patreon dan Parallaxa Pro Pack (paket konten berbayar terpisah); plugin ini sendiri tetap gratis dan lengkap.

## 概要（日本語）

**Parallaxa** は Parallaxa Labs による 2D VTuber 向けの無料 OBS Studio プラグイン（GPL-2.0-or-later）です。レイヤーの奥行きと視差、仮想カメラ（ズーム、パン、回転、揺れ、自動ドリフト、音声連動）、ワンパスのカラールックとエフェクト、レイヤーごとの被写界深度ぼかし、操作ドック、obs-websocket のベンダー API（`parallaxa`）を備えています。ガイド: [docs/user/ja/guide.md](docs/user/ja/guide.md)。ビルド: `tools/build.ps1`（MSVC）、リリースパッケージ: `release/package.ps1`。`docs/protocol` のプロトコル文書は MIT ライセンスです。開発は Patreon と Parallaxa Pro Pack（別売りのコンテンツパック）で支えられていますが、プラグイン本体は今後も無料で全機能を使えます。
