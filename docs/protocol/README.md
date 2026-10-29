# docs/protocol — Parallaxa vendor protocol

> License: **MIT** (`LICENSE` in this folder), unlike the rest of this repository (GPL-2.0-or-later). Anyone may implement clients or servers of this protocol, including proprietary ones.

These files define how other programs control the Parallaxa plugin over obs-websocket (vendor `parallaxa`) and the formats of its preset files. They are normative: when the plugin and these documents disagree, the plugin is fixed.

| File | Contents |
|---|---|
| [vendor-api.md](vendor-api.md) | The obs-websocket vendor `parallaxa`, protocol 1: requests, responses, events, error codes, depth and solver math, UI depth mapping, move / recipe / FX preset files, UX mapping |
| `schemas/vendor.common.json`, `vendor.requests.json`, `vendor.responses.json`, `vendor.events.json` | JSON Schemas of the vendor messages |
| `schemas/move.json`, `recipe.json`, `fx-preset.json` | JSON Schemas of the preset files (`data/presets`) |
| [names.vendor.json](names.vendor.json) | Generated: every string constant of the vendor protocol, grouped (constant name → value) |
| [vendor-tables.json](vendor-tables.json) | Generated: the ordered vendor enums, numeric limits and C buffer sizes |
| [builtin-presets.json](builtin-presets.json) | Generated from `data/presets`: ids of the free presets the plugin ships (recipes, moves, FX presets with their params, LUTs) |

The schemas use a subset of JSON Schema 2020-12 (`$ref` across files, type, const, enum, numeric and length limits, required, additionalProperties); `tests/contracts/schema.mjs` validates it.

## Mirrors and checks

- C: `include/stage-protocol.h`, checked against `names.vendor.json` and `vendor-tables.json` by `tests/contracts/protocol_check.mjs` (ctest `protocol_parity`).
- TypeScript: `dock-src/src/lib/protocol/vendor.ts`, used by the standalone dock.
- Presets: `tests/contracts/presets_check.mjs` validates every file under `data/presets` against the schemas and keeps `builtin-presets.json` up to date.

```powershell
ctest --test-dir build -R "protocol_parity|presets_check" --output-on-failure
node tests/contracts/presets_check.mjs           # check every preset under data/presets
node tests/contracts/presets_check.mjs --write   # after changing a free preset: rewrite builtin-presets.json
```

Every ` ```json ` block in `vendor-api.md` is preceded by `<!-- example: <name> schema: <file>#<pointer> -->` (or `<!-- schema: ... -->`) and validates against that schema. Blocks tagged `jsonc` are illustrative only.

## Changes

Parallaxa Labs maintains these documents together with AI Live Studio, which implements the same protocol, and publishes them here; `names.vendor.json` and `vendor-tables.json` are generated from that reference set of constants. A change to the protocol is documented here first and lands in the C header and the TypeScript mirror in the same change. Contributions to this folder are accepted under the MIT licence (see CONTRIBUTING.md).

## Versioning

The vendor protocol is version 1. Additive changes (new optional fields, requests, events, enum values) keep the version and are documented here first; breaking changes bump it.
