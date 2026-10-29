# Parallaxa built-in presets

Formats: `docs/protocol/vendor-api.md` section 6 and the schemas `move.json`, `recipe.json`, `fx-preset.json` in `docs/protocol/schemas`. Each file is `<id>.json`; ids match `^[a-z0-9][a-z0-9_-]{0,30}$`; every name and description is localised `{en, ja, id}`.

## License

All files in this folder are original works, Copyright (C) 2026 Parallaxa Labs, made for Parallaxa. The moves, recipes and FX presets were written by hand. The LUTs are generated procedurally from the parameters in `luts/README.md`. The two sample images (`samples/stage_sample_bg.png`, `samples/stage_sample_fg.png`) were drawn for Parallaxa. No third-party presets, LUTs or images are included. Everything here ships with the plugin under GPL-2.0-or-later (`LICENSE`) and is free to use.

The schemas keep an optional `"pro"` flag for presets of the Parallaxa Pro Pack, a separate, paid content download in the same formats that uses its own `pro_*` ids (see "Supporting Parallaxa" in the top-level README). No preset in this folder sets it: recipes carry `"pro": false` because the recipe schema requires the field, and moves and FX presets leave it out.

## Contents

| folder | basics | more looks |
|---|---|---|
| `moves/` | `zoom_in`, `zoom_out`, `pan_left`, `pan_right`, `shake`, `punch`, `dolly`, `nod`, `reset` (the 9 built-ins) | cinematic singing moves: `slow_push` (8 s verse push-in), `pull_reveal` (outro pull-back), `crane_up` (bridge rise), `sway_orbit` (ballad sway with roll), `chorus_lift` (anticipation then push for the chorus), `dutch_tilt` (6 degree drama tilt) |
| `recipes/` | `clean` (no motion, no FX: the "off" look), `cozy_chat` (default look), `karaoke_spotlight`, `brb`, `product_showcase` (neutral FX; AI Live Studio uses it as its default promo look) | the look presets `talk_show`, `singing_ballad`, `singing_hype`, `gaming_facecam`, `late_night_lofi`, `spotlight`, plus `rainy_lofi`, `hype_raid`, `horror` |
| `fx/` | `neutral` (all neutral, filter skips itself), `cozy`, `film`, `dreamy`, `horror`, `hype` | `spotlight`, `crisp`, `rainy`, `lofi` |
| `luts/` | `identity`, `film`, `night` (512x512 PNG, 64^3; see `luts/README.md`) | |
| `samples/` | importable OBS scene collection with original background and foreground art | |

The nine standard looks are `clean`, `cozy_chat`, `talk_show`, `singing_ballad`, `singing_hype`, `gaming_facecam`, `late_night_lofi`, `spotlight`, `product_showcase` (ids recommended in vendor-api.md section 10). AI Live Studio uses `brb` for its BRB screen and `product_showcase` for its promo segments by default.

Conventions used by the moves: `dx`/`dy` are fractions of the canvas, small (at most 0.08) so live-safe overscan hides the edges; camera moves that should stay put use `hold: true`, impacts (`shake`, `punch`, `nod`) end at zero and spring back; rolls stay within the live-safe limit of 8 degrees. Recipes keep `bg` 3 / `char` 1 / `fg` 0.6 except `gaming_facecam` (`bg` 1.6, no depth blur, so a game capture stays sharp).

## Validate

```powershell
node tests/contracts/presets_check.mjs           # schemas, ids, cross references, LUTs, sample collection
node tests/contracts/presets_check.mjs --write   # after adding/removing a preset: refresh docs/protocol/builtin-presets.json
```

Both run in `ctest` (`presets_check`) when Node.js is on PATH.

## Sample scene collection (`samples/`)

`parallaxa_sample.json` is an OBS scene collection with one scene, **Parallaxa Sample - Just Chatting**:

| layer (bottom to top) | source | Parallaxa role / depth |
|---|---|---|
| Background | image `stage_sample_bg.png` (cozy room at night, 1920x1080) with a `stage_depth_blur` filter | `bg` / 3.0 |
| VTube Studio (Spout2) | `spout_capture`, sender `VTubeStudioSpout`, fit to the canvas | `char` / 1.0 |
| Foreground | image `stage_sample_fg.png` (desk edge, plant, mug; transparent) | `fg` / 0.6 |

The scene carries the `stage_camera_rig` and `stage_fx` filters with default settings; depth and role are stored in each item's private settings (`stage.depth`, `stage.role`).

Import it (1920x1080 canvas):

1. Install Parallaxa and, for the avatar, obs-spout2-plugin (1.13.0 for OBS 32, 1.14.0 for OBS 33). In VTube Studio, turn on Spout2 output.
2. OBS: **Tools > Parallaxa: Import sample scene**. The scene is added to the current scene collection with the image paths made absolute in this installed `samples` folder (`src/presets/stage-sample.c`), so there is no Missing Files step. Names already in use get a number ("Background 2") and every source gets a new uuid: nothing existing is overwritten and the sample can be imported again. In studio mode the new scene becomes the preview; the program scene never changes.
3. Open the scene's filters and pick a look preset, or apply one from the Parallaxa dock.

Without the Tools menu: **Scene Collection > Import**, pick `parallaxa_sample.json`, tick it, **Import**, select **Scene Collection > Parallaxa Sample**. OBS then reports the two images as missing (the file keeps them relative); in the **Missing Files** dialog choose **Search Directory...**, select this `samples` folder, then **Apply**.

The Spout2 source settings key (`spoutsenders`) follows obs-spout2-plugin; if the avatar does not appear, pick the sender in the source properties.
