# Contributing to Parallaxa

Thank you for helping. Bug reports, fixes, translations, presets and docs are all welcome.

## Before you start

- **Bugs:** open an issue with your OBS version, Windows version, the steps to reproduce and the lines of the OBS log that start with `[parallaxa]` (**Help > Log Files > View Current Log**).
- **Features:** open an issue first, so we can agree on the approach before you write code.
- **Security problems:** do not open a public issue; use the repository's private vulnerability reporting.

## Development setup

See "Build" in [README.md](README.md). In short: Visual Studio 2022 Build Tools (C++), OBS Studio 32.x, Node.js 22.12+, pnpm 11.

```powershell
powershell -NoProfile -File tools/build.ps1          # build + ctest
pnpm -C dock-src verify                              # when you touch dock-src/ or the presets
powershell -NoProfile -File release/check.ps1        # scripts, installer, layout rules, contracts
```

Pull requests must pass CI (`.github/workflows/ci.yml`): build, every ctest (unit, GPU, vendor, contract checks), the dock rebuild check and the release lint.

## Code style

- C17, MSVC, no Qt, no new dependencies. OBS style: tabs (8 wide), `snake_case`, the `stage_` prefix for internal symbols; `.editorconfig` has the rest.
- Keep the render path cheap: the camera rig moves scene items and adds no render pass; FX is a single pass. Measure before you add work per frame.
- Every user-visible string goes through the locale files. Add new keys to `docs/ux/stage-plugin-ux.md` section 8 and regenerate with `py tools/gen-locale.py` (ctest `core_locale_check` fails when they drift).
- Logic gets a test (`tests/` or the module's `tests/` folder).

## Protocol changes

`docs/protocol` is the contract with every client and is MIT-licensed. Additive changes only within protocol version 1 (new optional fields, requests, events, enum values), documented in `docs/protocol/vendor-api.md` first. Change `include/stage-protocol.h` together with `docs/protocol/names.vendor.json` and `docs/protocol/vendor-tables.json`; `tests/contracts/protocol_check.mjs` must pass. Maintainers coordinate the matching update of client mirrors.

## Presets

Presets are JSON files in `data/presets/{moves,recipes,fx}` (formats in `docs/protocol/vendor-api.md` section 6, schemas in `docs/protocol/schemas`); every name and description is localised `{en, ja, id}`. Run `node tests/contracts/presets_check.mjs --write` after adding or removing one. Only submit presets you made yourself.

## Dock sources (`dock-src/`)

`dock-src/` is the source of `data/dock/dock-standalone.html`. Commit the rebuilt HTML (`pnpm -C dock-src build`) with every source change; CI checks they match. Most files in `dock-src/` are shared with AI Live Studio, a closed-source Parallaxa Labs product, so maintainers port accepted changes there by hand. For that reason every outside contribution to `dock-src/` needs a signed contributor licence agreement ([CLA.md](CLA.md)) before it can be merged; see below.

## Licence of contributions

By submitting a contribution you agree that it is licensed under the licence of the files you change: GPL-2.0-or-later for the plugin, the dock and the presets, MIT for `docs/protocol`. Add a `Signed-off-by:` line to your commits ([Developer Certificate of Origin](https://developercertificate.org/)).

- **`dock-src/`:** signing [CLA.md](CLA.md) is required. It lets Parallaxa Labs use your change in AI Live Studio as well. Pull requests that change `dock-src/` are not merged until the author has signed.
- **`docs/protocol/`:** contributions are accepted under the MIT licence of that folder; no CLA is needed.
- **Everything else:** GPL-2.0-or-later, no CLA needed.
