# Parallaxa — obs-websocket vendor protocol v1

> License: **MIT** (this document and `docs/protocol/schemas/*`). Anyone may implement clients or servers of this protocol, including proprietary ones. The Stage plugin itself is GPL-2.0-or-later.
>
> Status: normative for protocol version **1**. Mirrors: `include/stage-protocol.h` in Parallaxa (C), checked against `names.vendor.json` and `vendor-tables.json`; `dock-src/src/lib/protocol/vendor.ts` in Parallaxa (TypeScript, the standalone dock); AI Live Studio keeps its own Go and TypeScript mirrors, checked against this folder. Schemas: `schemas/vendor.common.json`, `vendor.requests.json`, `vendor.responses.json`, `vendor.events.json`, `move.json`, `recipe.json`, `fx-preset.json`.

## 1. Transport

Stage registers the obs-websocket **vendor** `parallaxa` from `obs_module_post_load`. Vendor names are global in obs-websocket, so the name is branded. If obs-websocket is missing or disabled (and in OBS 33 Safe Mode), registration fails; the plugin logs one warning and keeps working without remote control.

Clients send obs-websocket v5 `CallVendorRequest` (op 6):

```jsonc
{"requestType":"CallVendorRequest","requestId":"1",
 "requestData":{"vendorName":"parallaxa","requestType":"camera_move","requestData":{"move":"zoom_in"}}}
```

and read the reply from the `RequestResponse` (op 7). For `CallVendorRequest`, obs-websocket **wraps** the vendor's answer: `d.responseData` is `{vendorName, requestType, responseData}`, so the Stage payload described in this document is at **`d.responseData.responseData`** (one level deeper than for ordinary requests). First check `d.requestStatus.result`: when it is false (for example code 600 `ResourceNotFound`, "No vendor was found by that name", because Stage is not loaded), there is no vendor payload at all.

```jsonc
{"op":7,"d":{"requestType":"CallVendorRequest","requestId":"1",
 "requestStatus":{"result":true,"code":100},
 "responseData":{"vendorName":"parallaxa","requestType":"camera_move",
                 "responseData":{"ok":true,"id":42}}}}
```

Events arrive as obs-websocket `VendorEvent` (op 5, event subscription `Vendors` = `1<<9`) with `{vendorName:"parallaxa", eventType, eventData}`.

Every `responseData` contains `ok` (bool). On failure it contains `code` and `error` (a short English message for logs, never shown verbatim to viewers):

<!-- example: vendor.error.res schema: vendor.responses.json#/$defs/error -->
```json
{"ok": false, "code": "not_found", "error": "unknown move 'spin'"}
```

### 1.1 obs_data limits (no null, no arrays of scalars in responses)

obs-websocket hands vendor payloads to and from the plugin as libobs `obs_data`, not as JSON. `obs_data` arrays hold objects only, and a null object is serialized as `{}`. Responses and events of this protocol therefore **never contain `null` and never contain an array of strings or numbers**: "no value" is an **absent field**. Three response fields are optional for this reason:

| Field | Rule |
|---|---|
| `get_info.features` | Optional. Parallaxa never sends it over obs-websocket. Absent means "not reported", not "no features": clients learn what is available from `rigs`, `fx` and the error codes (`no_rig`, `no_fx`). Other transports (test doubles) may send it. |
| `recipe_list.recipes[].tags` | Optional. Parallaxa never sends it over obs-websocket; clients treat absent as no tags. The recipe files (section 6.2) keep their `tags`. |
| `layer_list.layers[].depth` | Present exactly when the item is enrolled (`role` is not `none`); absent for items that are not enrolled. Never `null`. |

Requests are not affected in the same way: obs-websocket passes a JSON `null` in `requestData` to the vendor as a null object item, which the plugin reads as `null` (`layer_set` `depth: null`, section 4.11).

## 2. Conventions

- **`scene`** is optional in every request that targets a rig. It accepts a scene **name or uuid**. Empty or absent means the current **program** scene (cached by the plugin from `OBS_FRONTEND_EVENT_SCENE_CHANGED`, never queried from the graphics thread). For `fx_set` / `fx_pulse` it names the scene or source that carries the `stage_fx` filter.
- **Validation is strict.** Unknown fields, wrong types and out-of-range values return `bad_param`; the plugin never guesses. The only silent clamping is (a) the result of a `relative` `camera_set`, and (b) the Live-safe caps of the rig (section 9).
- **Ids** (moves, recipes, FX presets, LUTs, light leaks) match `^[a-z0-9][a-z0-9_-]{0,30}$` so they fit `char[32]`. Network payloads never carry file paths: a LUT or leak is referenced by id and resolved inside `data/presets/luts/` or the user preset folder.
- **Commands** (everything that changes state) are validated on the obs-websocket thread, pushed onto the plugin command queue and answered immediately with `{ok:true, id}`. The `id` is a positive 64-bit integer, unique per OBS session, and is echoed by `move_finished`. Commands are applied on the next graphics tick (≤ 16.7 ms at 60 fps).
- **Units.** Camera `x`/`y` are canvas pixels (0 = neutral; positive x pans the view right, positive y pans down). `zoom` 1 = neutral. `rot` is degrees (positive = clockwise). Durations are integer milliseconds. Amounts and intensities are 0..1 unless stated.
- **Defaults.** An absent optional field means "use the default / leave unchanged" as stated per request. `ease` defaults to `ease_in_out_cubic`.
- Clients must ignore unknown fields in responses and events (forward compatibility). Servers must not add fields without bumping the docs and schemas.

## 3. Depth model and solver math

Each scene item enrolled by the rig stores **private settings** (saved with the scene collection):

| key | type | meaning |
|---|---|---|
| `stage.depth` | double 0.1–10 | 1.0 = character plane, > 1 behind (background), < 1 in front (foreground) |
| `stage.role` | string | `none`, `bg`, `char`, `fg`, `sky`, `static` |
| `stage.rest` | object `{px,py,sx,sy,rot}` | the user's own transform (rest pose) |

Saved views live in the **scene's** private settings under `stage.views` (section 7).

**Roles.** `bg`, `char` and `fg` use parallax factor `f = 1 / clamp(d, 0.1, 10)`, continuous over the whole range (no jump at 10). `sky` (infinitely far backdrop) and `static` (frames, HUD, pinned to screen) use `f = 0` explicitly, so "infinitely far" is a role, never a depth value. `none` means "not enrolled": the rig never touches the item. Groups and nested scenes count as one layer; their children are never touched.

Default depth when a role is set on an item without depth: `bg` 3.0, `char` 1.0, `fg` 0.6, `sky` 10, `static` 1.0.

**Solver.** Camera `c = {x, y, zoom, rot}`, canvas centre `C`, layer factor `f`:

```
s      = clamp(zoom ^ f, 0.25, 8)          (STAGE_LAYER_SCALE_MIN / MAX)
pos    = C + R(rot * f) * ((rest.pos - C) * s) - (x, y) * f
scale  = rest.scale * s            (or bounds * s when the item uses a bounds type)
rot'   = rest.rot + rot * f
inverse: rest.pos = C + R(-rot*f) * (pos + (x,y)*f - C) / s ; rest.scale = scale / s ; rest.rot = rot' - rot*f
```

**Layer zoom factor.** `s` is multiplicative in the camera zoom: `s = 1` for `f = 0` (sky, static), `s = zoom` for `f = 1` (the character plane follows the camera exactly), and it grows or shrinks geometrically for nearer or farther layers. Because `zoom ∈ [0.5, 4]` is positive, `zoom ^ f` is strictly positive for every `f ∈ [0, 10]`, so no layer can collapse to zero or flip, and the inverse never divides by zero. The clamp to `[0.25, 8]` bounds extreme foreground layers (for example depth 0.1 at zoom 0.5 gives `0.5^10 ≈ 0.001`, clamped to 0.25; depth 0.5 at zoom 0.5 gives exactly 0.25; the default fg depth 0.6 at zoom 0.5 gives ≈ 0.31). `s` is monotone in `f`, so parallax ordering is preserved. Reference implementations: `stage_layer_scale()` (C), `LayerScale` (Go), `layerScale` (TS). An earlier draft used the linear form `1 + (zoom − 1) · f`, which reaches 0 or goes negative inside the documented ranges and must not be used.

Items whose `bounds_type != OBS_BOUNDS_NONE` are scaled through their bounds, not their scale. The `bg` role is auto-oversized by the **overscan** fraction (default 0.08) and the camera is clamped so the canvas stays covered (Live-safe).

**UI depth slider.** UIs show depth as `v ∈ [-100, +100]` (−100 far, 0 = character plane, +100 near). The mapping is

```
d = clamp(10^(-v / 100), 0.1, 10)        v = -100 * log10(clamp(d, 0.1, 10))
```

so `v = 0 → d = 1`, `v = -100 → d = 10`, `v = +100 → d = 0.1` exactly at the slider ends, and every step of the travel moves the layer (`v = -50 → d ≈ 3.16`, `v = +50 → d ≈ 0.32`). The role defaults sit at bg 3 → `v ≈ -48`, char 1 → 0, fg 0.6 → `v ≈ +22`. This is a UI mapping only: the wire value `layer_set.depth` is unchanged. An earlier draft used a scale of 50, which saturated past ±50 so half the travel did nothing; it must not be used. Constants: `STAGE_UI_DEPTH_MIN/MAX/SCALE` (C), `UIDepthMin/Max/Scale`, `DepthFromUI`, `UIFromDepth` (Go), `depthFromUi`, `uiFromDepth` (TS).

**Depth of field.** The rig owns `focus_depth` (0.1–10, default 1) and `amount` (0–1, default 0). On every tick where either changed, for each layer with role `bg`, `fg` or `sky` (sky uses depth 10) that has a filter of id `stage_depth_blur` on its source, the rig sets the double setting `auto_strength` to `clamp(|depth − focus_depth| × amount, 0, 1)`. `auto_strength = 0` makes the blur filter call `obs_source_skip_video_filter` (zero cost). A rack focus is `dof_set` with `focus_depth` and a `duration_ms`.

## 4. Requests

| requestType | requestData | responseData |
|---|---|---|
| `get_info` | `{}` | `{ok, version, protocol, obs, canvas, features?, rigs, fx}` |
| `get_state` | `{scene?}` | `{ok, scene, scene_uuid, camera, auto, fx, dof, moving, move?, frozen, view}` |
| `camera_move` | `{scene?, move, duration_ms?, intensity?, ease?}` | `{ok, id}` |
| `camera_set` | `{scene?, x?, y?, zoom?, rot?, relative?, duration_ms?, ease?}` | `{ok, id}` |
| `camera_reset` | `{scene?, duration_ms?}` | `{ok, id}` |
| `camera_auto` | `{scene?, mode, intensity?}` | `{ok, id}` |
| `fx_set` | `{scene?, preset?, params?, duration_ms?}` | `{ok, id}` |
| `fx_pulse` | `{scene?, effect, intensity?, duration_ms?}` | `{ok, id}` |
| `dof_set` | `{scene?, focus_depth?, amount?, duration_ms?, ease?}` | `{ok, id}` |
| `layer_list` | `{scene?}` | `{ok, scene, scene_uuid, layers}` |
| `layer_set` | `{scene?, item_id, depth?, role?}` | `{ok, id}` |
| `move_list` | `{}` | `{ok, moves}` |
| `recipe_list` | `{}` | `{ok, recipes}` |
| `recipe_apply` | `{scene?, recipe, duration_ms?}` | `{ok, id, applied}` |
| `view_save` | `{scene?, slot, name?, transition?, duration_ms?, keep_pose?}` | `{ok, view}` |
| `view_apply` | `{scene?, slot, transition?, duration_ms?}` | `{ok, id}` |
| `view_list` | `{scene?}` | `{ok, scene, scene_uuid, views}` |
| `view_delete` | `{scene?, slot}` | `{ok}` |
| `freeze` | `{scene?, on?}` | `{ok, frozen}` |
| `panic` | `{}` | `{ok}` |

Requests that target a scene answer `no_rig` when that scene has no enabled `stage_camera_rig` filter (`fx_set`/`fx_pulse` answer `no_fx` when the target has no `stage_fx`), and `not_found` when the scene name/uuid does not exist.

### 4.1 `get_info`

Plugin and OBS versions, the canvas size, every rig and every FX filter. The optional feature list `features` (`camera`, `fx`, `audio`, `dof`, `views`) is never sent by Parallaxa over obs-websocket (section 1.1).

<!-- schema: vendor.requests.json#/$defs/get_info -->
```json
{}
```

<!-- example: vendor.get_info.res schema: vendor.responses.json#/$defs/get_info -->
```json
{
  "ok": true,
  "version": "0.1.0",
  "protocol": 1,
  "obs": "32.2.2",
  "canvas": {"width": 1920, "height": 1080},
  "rigs": [{"scene": "Just Chatting", "scene_uuid": "6f1c2a9e-3b7d-4c55-9a1e-0d2b8f4e7a10", "enabled": true}],
  "fx": [{"source": "Just Chatting", "source_uuid": "6f1c2a9e-3b7d-4c55-9a1e-0d2b8f4e7a10", "enabled": true}]
}
```

### 4.2 `get_state`

<!-- example: vendor.get_state.req schema: vendor.requests.json#/$defs/get_state -->
```json
{"scene": "Just Chatting"}
```

`move` is present only while a move runs. `view` is the slot last applied while the camera has not been changed since (else 0).

<!-- example: vendor.get_state.res schema: vendor.responses.json#/$defs/get_state -->
```json
{
  "ok": true,
  "scene": "Just Chatting",
  "scene_uuid": "6f1c2a9e-3b7d-4c55-9a1e-0d2b8f4e7a10",
  "camera": {"x": -12.5, "y": 4, "zoom": 1.08, "rot": 0, "shake": 0},
  "auto": {"mode": "drift", "intensity": 0.35},
  "fx": {"preset": "cozy"},
  "dof": {"focus_depth": 1, "amount": 0},
  "moving": true,
  "move": "zoom_in",
  "frozen": false,
  "view": 0
}
```

### 4.3 `camera_move`

Plays a move (section 6): a built-in (`zoom_in`, `zoom_out`, `pan_left`, `pan_right`, `shake`, `punch`, `dolly`, `nod`, `reset`) or a custom id from `data/presets/moves` or the user folder. `duration_ms` 50–10000 overrides the move's own duration; `intensity` 0–2 (default 1) scales every delta; `ease` replaces every key's ease. A new move cancels the running one (its `move_finished` has `cancelled:true`). Unknown move → `not_found`. Manual commands pause the auto camera for 10 s, then auto blends back in.

<!-- example: vendor.camera_move.req schema: vendor.requests.json#/$defs/camera_move -->
```json
{"move": "zoom_in", "duration_ms": 800, "intensity": 1.2, "ease": "ease_out_cubic"}
```

<!-- example: vendor.cmd.res schema: vendor.responses.json#/$defs/cmd -->
```json
{"ok": true, "id": 42}
```

### 4.4 `camera_set`

Moves the camera target to an absolute pose; omitted fields keep their current target. Absolute `zoom` must be 0.5–4 and `rot` −45–45. With `relative:true` the values are **added** to the current target (a nudge) and the result is clamped to the camera range. `duration_ms` 0–10000 (default 0 = follow through the rig's spring smoothing only). The Stage dock's drag pad sends `duration_ms:0` while dragging and an eased set on release.

<!-- example: vendor.camera_set.req schema: vendor.requests.json#/$defs/camera_set -->
```json
{"scene": "Just Chatting", "x": 120, "y": -40, "zoom": 1.5, "rot": 0, "duration_ms": 600, "ease": "ease_in_out_cubic"}
```

<!-- example: vendor.camera_set_relative.req schema: vendor.requests.json#/$defs/camera_set -->
```json
{"x": -24, "zoom": -0.1, "relative": true}
```

### 4.5 `camera_reset`

Returns the camera to home `{x:0, y:0, zoom:1, rot:0}` over `duration_ms` (default 600) and cancels the running move. The auto mode is unchanged.

<!-- example: vendor.camera_reset.req schema: vendor.requests.json#/$defs/camera_reset -->
```json
{"duration_ms": 400}
```

### 4.6 `camera_auto`

Sets the auto camera mode: `off`, `drift`, `chatting`, `singing`. `intensity` 0–1 (absent = keep current; initial 0.35).

<!-- example: vendor.camera_auto.req schema: vendor.requests.json#/$defs/camera_auto -->
```json
{"mode": "chatting", "intensity": 0.5}
```

### 4.7 `fx_set`

Applies an FX preset by id (`not_found` if unknown) and/or individual `params` on top of it, tweened over `duration_ms` (default 0). At least one of `preset` and `params` is required. All parameters at their neutral value make the filter skip itself (`obs_source_skip_video_filter`, bit-identical output, zero cost). `bloom > 0` enables the optional half-resolution bloom passes.

| param | range | neutral | meaning |
|---|---|---|---|
| `exposure` | −2..2 | 0 | EV |
| `contrast`, `saturation`, `temperature`, `tint` | −1..1 | 0 | grade |
| `vignette` | 0..1 | 0 | amount |
| `vignette_soft` | 0..1 | 0.5 | softness |
| `vignette_round` | 0..1 | 1 | roundness |
| `chroma` | 0..1 | 0 | radial chromatic aberration |
| `grain` | 0..1 | 0 | film grain amount |
| `grain_size` | 0.5..4 | 1 | grain size |
| `lut` | id or `""` | `""` | 64³ LUT strip (512×512 PNG) |
| `lut_amount` | 0..1 | 0 | LUT mix |
| `leak` | id or `""` | `""` | light-leak texture |
| `leak_amount` | 0..1 | 0 | screen-blend amount |
| `leak_speed` | 0..2 | 0.2 | UV scroll speed |
| `bloom` | 0..1 | 0 | bloom amount (0 = no extra passes) |
| `bloom_threshold` | 0..1 | 0.8 | bright-pass threshold |

<!-- example: vendor.fx_set.req schema: vendor.requests.json#/$defs/fx_set -->
```json
{"preset": "cozy", "params": {"vignette": 0.3, "lut": "film", "lut_amount": 0.6, "bloom": 0.25}, "duration_ms": 500}
```

### 4.8 `fx_pulse`

One-shot pulse: `flash`, `chroma_kick`, `vignette_pulse`; `intensity` 0–1 (default 1), `duration_ms` 50–2000 (default 300), decaying to 0. The photosensitivity cap allows at most **3 flashes per second**; extra pulses inside the window are coalesced (still answered `ok`).

<!-- example: vendor.fx_pulse.req schema: vendor.requests.json#/$defs/fx_pulse -->
```json
{"effect": "flash", "intensity": 0.6, "duration_ms": 250}
```

### 4.9 `dof_set`

Sets depth of field (section 3). `focus_depth` 0.1–10, `amount` 0–1, at least one required; `duration_ms` + `ease` animate it (rack focus).

<!-- example: vendor.dof_set.req schema: vendor.requests.json#/$defs/dof_set -->
```json
{"focus_depth": 3, "amount": 0.5, "duration_ms": 1200, "ease": "ease_in_out_quint"}
```

### 4.10 `layer_list`

Every item of the scene. `depth` is present only for enrolled items; it is absent (never `null`, section 1.1) for items that are not enrolled (`role: "none"`). `depth_blur` says whether the item's source carries a `stage_depth_blur` filter.

<!-- schema: vendor.requests.json#/$defs/layer_list -->
```json
{}
```

<!-- example: vendor.layer_list.res schema: vendor.responses.json#/$defs/layer_list -->
```json
{
  "ok": true,
  "scene": "Just Chatting",
  "scene_uuid": "6f1c2a9e-3b7d-4c55-9a1e-0d2b8f4e7a10",
  "layers": [
    {"item_id": 1, "name": "BG", "source_uuid": "0b6d1f7c-5e2a-4d8b-8c1f-2a9e7b3c4d51", "depth": 3, "role": "bg", "visible": true, "locked": false, "depth_blur": true},
    {"item_id": 2, "name": "VTube Studio", "source_uuid": "c4e8a2d6-7f1b-4a3c-9e5d-6b2f8a1c3e72", "depth": 1, "role": "char", "visible": true, "locked": false, "depth_blur": false},
    {"item_id": 3, "name": "FG Desk", "source_uuid": "9a3f5c7e-1d2b-4e6a-8f0c-3b5d7e9a1c24", "depth": 0.6, "role": "fg", "visible": true, "locked": false, "depth_blur": true},
    {"item_id": 4, "name": "Chat Box", "source_uuid": "5d7e9a1c-3b5d-4f0c-8e6a-1d2b3f5c7e93", "role": "none", "visible": true, "locked": true, "depth_blur": false}
  ]
}
```

### 4.11 `layer_set`

Enrols or edits one item. `depth` 0.1–10, or `null` to **un-enrol** (restores the rest pose and removes the `stage.*` keys). `role` sets the role; setting a role on an un-enrolled item enrols it at the role's default depth; `role:"none"` un-enrols like `depth:null`. At least one of `depth` and `role` is required. Unknown `item_id` → `not_found`.

<!-- example: vendor.layer_set.req schema: vendor.requests.json#/$defs/layer_set -->
```json
{"item_id": 3, "depth": 0.6, "role": "fg"}
```

<!-- example: vendor.layer_set_null.req schema: vendor.requests.json#/$defs/layer_set -->
```json
{"scene": "Just Chatting", "item_id": 4, "depth": null}
```

### 4.12 `move_list` and `recipe_list`

<!-- example: vendor.move_list.res schema: vendor.responses.json#/$defs/move_list -->
```json
{
  "ok": true,
  "moves": [
    {"id": "zoom_in", "name": {"en": "Zoom In", "ja": "ズームイン", "id": "Perbesar"}, "duration_ms": 800, "builtin": true},
    {"id": "orbit_slow", "name": {"en": "Slow Orbit", "ja": "スローオービット", "id": "Orbit Pelan"}, "duration_ms": 4000, "builtin": false}
  ]
}
```

`user` marks recipes loaded from the user's config folder. `tags` is optional and never sent by Parallaxa over obs-websocket (section 1.1); the Looks UI must not depend on it.

<!-- example: vendor.recipe_list.res schema: vendor.responses.json#/$defs/recipe_list -->
```json
{
  "ok": true,
  "recipes": [
    {"id": "cozy_chat", "name": {"en": "Cozy Chat", "ja": "まったり雑談", "id": "Ngobrol Santai"}, "pro": false, "user": false},
    {"id": "my_cozy", "name": {"en": "My Cozy", "ja": "My Cozy", "id": "My Cozy"}, "pro": false, "user": true}
  ]
}
```

### 4.13 `recipe_apply`

Applies a recipe (section 6) as one atomic command sequence over `duration_ms` (default 1000): depths for items whose role is set or can be auto-guessed from the name (`bg|background|背景` → bg, `vts|spout|avatar|model|vtube` → char, `fg|foreground|前景` → fg), the auto mode, the FX preset, DoF, the optional start pose and overscan. `applied.layers` counts the items whose depth was set.

<!-- example: vendor.recipe_apply.req schema: vendor.requests.json#/$defs/recipe_apply -->
```json
{"scene": "Just Chatting", "recipe": "cozy_chat"}
```

<!-- example: vendor.recipe_apply.res schema: vendor.responses.json#/$defs/recipe_apply -->
```json
{"ok": true, "id": 43, "applied": {"layers": 3, "auto": "drift", "fx": "cozy"}}
```

## 5. Views, freeze and panic

### 5.1 Saved views (camera presets 1–9)

A view is an **absolute** camera pose stored per scene in the scene's private settings (`stage.views`), slots 1–9. `view_save` stores the current camera **target** (not a mid-transition pose), overwriting the slot, and the reply's `view` is exactly what was stored. The target is a running transition's destination; during a move it is the pose the move started from (a `hold:true` move's end pose becomes the target when the move ends). It is read from the rig's last published state, so a camera command sent in the same frame is not seen yet. The slot is stored before the rig is told, so a full queue does not fail the save; `name` defaults to `"View <slot>"` and is limited to **47 Unicode code points** (not bytes, not UTF-16 units: UIs count with `[...name].length`), valid UTF-8 without NUL; the plugin stores it in `char[STAGE_VIEW_NAME_MAX]` (192 bytes, enough for 47 four-byte code points plus NUL), checks it with `stage_view_name_valid()` and answers `bad_param` for anything longer, never truncating, `transition` to `smooth`, `duration_ms` to 600 when omitted (an explicit 0 is stored as 0). `view_apply` moves to the slot using its own transition unless overridden; an empty slot → `not_found`. Transitions: `cut` (duration 0), `smooth` (`ease_in_out_cubic`), `whip` (`ease_in_out_quint`), `punch` (`ease_out_back`).

**Editing a view.** `keep_pose: true` (optional boolean, default `false`) edits a stored view in place: the stored camera pose is kept, not replaced by the current camera, and `name`, `transition` and `duration_ms` keep their stored values when omitted (no defaults are applied). An empty slot → `not_found`. The edit is one read-modify-write under the views lock, and the rig re-stores the kept pose as is (not clamped to the current canvas). The reply's `view` is the stored result, as for any save. UIs use it to rename a view or change its transition, and send the default name (`View <slot>`) when the user clears the name, since an omitted name is kept; "Save current camera here" sends no `keep_pose`. A plugin older than this field answers `bad_param` (unknown key); UIs say Parallaxa needs an update and never fall back to a plain save, which would overwrite the pose. The field is optional, so the protocol version stays 1.

<!-- example: vendor.view_save.req schema: vendor.requests.json#/$defs/view_save -->
```json
{"slot": 1, "name": "Wide", "transition": "smooth", "duration_ms": 600}
```

<!-- example: vendor.view_save.res schema: vendor.responses.json#/$defs/view_save -->
```json
{"ok": true, "view": {"slot": 1, "name": "Wide", "camera": {"x": 0, "y": 0, "zoom": 1, "rot": 0}, "transition": "smooth", "duration_ms": 600}}
```

<!-- example: vendor.view_save_keep_pose.req schema: vendor.requests.json#/$defs/view_save -->
```json
{"slot": 1, "name": "Wide shot", "transition": "whip", "keep_pose": true}
```

<!-- example: vendor.view_apply.req schema: vendor.requests.json#/$defs/view_apply -->
```json
{"slot": 2, "transition": "whip", "duration_ms": 350}
```

<!-- example: vendor.view_list.res schema: vendor.responses.json#/$defs/view_list -->
```json
{
  "ok": true,
  "scene": "Just Chatting",
  "scene_uuid": "6f1c2a9e-3b7d-4c55-9a1e-0d2b8f4e7a10",
  "views": [
    {"slot": 1, "name": "Wide", "camera": {"x": 0, "y": 0, "zoom": 1, "rot": 0}, "transition": "smooth", "duration_ms": 600},
    {"slot": 2, "name": "Close-up", "camera": {"x": 40, "y": -60, "zoom": 1.6, "rot": -2}, "transition": "punch", "duration_ms": 450}
  ]
}
```

<!-- example: vendor.view_delete.req schema: vendor.requests.json#/$defs/view_delete -->
```json
{"slot": 2}
```

<!-- example: vendor.ok.res schema: vendor.responses.json#/$defs/view_delete -->
```json
{"ok": true}
```

### 5.2 `freeze`

`on:true` freezes, `on:false` unfreezes, absent toggles. While frozen, automatic and cued motion is suppressed: auto modes, audio bounce and `camera_move` (answered `ok` and immediately finished with `cancelled:true`). Explicit positioning (`camera_set`, `camera_reset`, `view_apply`) is applied and the camera stays frozen at the new pose. `panic` clears the freeze.

<!-- example: vendor.freeze.req schema: vendor.requests.json#/$defs/freeze -->
```json
{"on": true}
```

<!-- example: vendor.freeze.res schema: vendor.responses.json#/$defs/freeze -->
```json
{"ok": true, "frozen": true}
```

### 5.3 `panic`

On **all** rigs and FX filters, bypassing queue limits: camera home over 200 ms, auto `off`, freeze cleared, FX to neutral, DoF amount 0, running moves cancelled. It is safe to call repeatedly.

<!-- schema: vendor.requests.json#/$defs/panic -->
```json
{}
```

## 6. Preset files

### 6.1 Moves — `data/presets/moves/<id>.json` (schema `move.json`)

User moves live in `obs_module_config_path("moves")`. Keys are evaluated per channel: a channel present in a key is keyed there (C mask bits `STAGE_CH_X|Y|ZOOM|ROT|SHAKE`); a channel absent from a key is interpolated through it. Key `t` (0–1, strictly increasing) is relative to `duration_ms`. `ease` is the ease **into** that key (default `ease_in_out_cubic`). Deltas: `dx`/`dy` are fractions of the canvas **width/height** (resolution independent), `dzoom` is added to zoom, `drot` degrees, `shake` 0–1 amplitude. `relative` (default true) applies deltas to the current camera; `relative:false` makes them absolute offsets from home. `hold:true` keeps the end pose; otherwise the camera springs back to where it started. Files with errors are logged and skipped; the file name must equal `<id>.json`.

<!-- example: preset.move schema: move.json -->
```json
{
  "id": "zoom_in",
  "name": {"en": "Zoom In", "ja": "ズームイン", "id": "Perbesar"},
  "duration_ms": 800,
  "relative": true,
  "hold": true,
  "tags": ["basic"],
  "keys": [
    {"t": 0},
    {"t": 1, "dzoom": 0.25, "ease": "ease_out_cubic"}
  ]
}
```

### 6.2 Recipes — `data/presets/recipes/<id>.json` (schema `recipe.json`)

A recipe is the UX **Look preset**. User recipes live in `obs_module_config_path("recipes")`. `roles` maps `bg`/`char`/`fg` to depths. Optional: `auto`, `fx` (FX preset id), `audio_react`, `dof`, `camera` (start pose), `overscan` (0–0.25), localised `description`.

<!-- example: preset.recipe schema: recipe.json -->
```json
{
  "id": "cozy_chat",
  "name": {"en": "Cozy Chat", "ja": "まったり雑談", "id": "Ngobrol Santai"},
  "description": {"en": "Gentle drift and warm grade for chatting.", "ja": "ゆったり揺れる暖かい雑談ルック。", "id": "Gerakan lembut dan warna hangat untuk ngobrol."},
  "pro": false,
  "tags": ["chat", "drift"],
  "roles": {"bg": 3, "char": 1, "fg": 0.6},
  "auto": {"mode": "drift", "intensity": 0.35},
  "fx": "cozy",
  "audio_react": {"amount": 0.25, "attack_ms": 30, "release_ms": 250},
  "dof": {"focus_depth": 1, "amount": 0.2},
  "overscan": 0.08
}
```

### 6.3 FX presets — `data/presets/fx/<id>.json` (schema `fx-preset.json`)

`params` uses the `fx_set` parameter set; absent parameters take the **neutral** value (a preset is absolute, unlike `fx_set` params). Preset `neutral` must exist and be all-neutral.

<!-- example: preset.fx schema: fx-preset.json -->
```json
{
  "id": "cozy",
  "name": {"en": "Cozy", "ja": "コージー", "id": "Hangat"},
  "tags": ["warm"],
  "params": {"exposure": 0.1, "contrast": 0.08, "saturation": 0.05, "temperature": 0.25, "vignette": 0.25, "vignette_soft": 0.6, "grain": 0.08, "bloom": 0.3, "bloom_threshold": 0.75}
}
```

## 7. Events

Emitted with `obs_websocket_vendor_emit_event` from a frontend tick callback.

| eventType | eventData | when |
|---|---|---|
| `state_changed` | same shape as `get_state` without `ok` | at most **10 Hz** per rig, only on change; stops when idle |
| `move_finished` | `{scene, scene_uuid, id, move, cancelled}` | a move ended or was superseded |
| `rig_changed` | `{scene, scene_uuid, enabled}` | a rig filter was added, removed, enabled or disabled |
| `perf_warning` | `{source, source_uuid, filter, cost_ms, budget_ms, window_s, paused}` | Stage render cost > 2 ms/frame for 5 s; the most expensive FX part was paused (`paused` = `bloom`, `lut`, `leak`, `grain`, `chroma`, or `""`) |
| `hotkey_forward` | `{action}` | an OBS hotkey "AI Live Studio: Stop speaking / Skip / Mute AI / Panic / Approve draft" was pressed: `ai_stop`, `ai_skip`, `ai_mute`, `ai_panic`, `ai_approve` |

<!-- example: vendor.state_changed.evt schema: vendor.events.json#/$defs/state_changed -->
```json
{
  "scene": "Just Chatting",
  "scene_uuid": "6f1c2a9e-3b7d-4c55-9a1e-0d2b8f4e7a10",
  "camera": {"x": 0, "y": 0, "zoom": 1.25, "rot": 0, "shake": 0},
  "auto": {"mode": "off", "intensity": 0.35},
  "fx": {"preset": ""},
  "dof": {"focus_depth": 1, "amount": 0},
  "moving": false,
  "frozen": true,
  "view": 1
}
```

<!-- example: vendor.move_finished.evt schema: vendor.events.json#/$defs/move_finished -->
```json
{"scene": "Just Chatting", "scene_uuid": "6f1c2a9e-3b7d-4c55-9a1e-0d2b8f4e7a10", "id": 42, "move": "zoom_in", "cancelled": false}
```

<!-- example: vendor.rig_changed.evt schema: vendor.events.json#/$defs/rig_changed -->
```json
{"scene": "Just Chatting", "scene_uuid": "6f1c2a9e-3b7d-4c55-9a1e-0d2b8f4e7a10", "enabled": false}
```

<!-- example: vendor.perf_warning.evt schema: vendor.events.json#/$defs/perf_warning -->
```json
{"source": "Just Chatting", "source_uuid": "6f1c2a9e-3b7d-4c55-9a1e-0d2b8f4e7a10", "filter": "stage_fx", "cost_ms": 2.6, "budget_ms": 2, "window_s": 5, "paused": "bloom"}
```

<!-- example: vendor.hotkey_forward.evt schema: vendor.events.json#/$defs/hotkey_forward -->
```json
{"action": "ai_panic"}
```

AI Live Studio relays these to its UI as `stage.state`, `stage.move_finished`, `stage.rig_changed`, `stage.perf_warning` and `stage.hotkey_forward` and maps `hotkey_forward` to its own actions: `ai_stop`→stop, `ai_skip`→skip, `ai_mute`→mute, `ai_panic`→panic, `ai_approve`→approve.

## 8. Error codes

| code | meaning | retry? |
|---|---|---|
| `bad_param` | wrong type, unknown field, or out of range | no |
| `no_rig` | the scene has no enabled Camera Rig | after adding the filter |
| `no_fx` | the target has no Stage FX filter | after adding the filter |
| `not_found` | unknown scene, item, move, recipe, preset, LUT, leak or empty view slot | no |
| `busy` | the command could not be queued (queue saturated or shutting down) | yes, with backoff |
| `internal` | unexpected plugin error (logged) | maybe |

## 9. Limits and Live-safe behaviour

| limit | value | C | Go |
|---|---|---|---|
| depth | 0.1–10 (char 1) | `STAGE_DEPTH_MIN/MAX/CHAR` | `DepthMin/Max/Char` |
| zoom / rot | 0.5–4 / −45–45° | `STAGE_ZOOM_*`, `STAGE_ROT_*` | `ZoomMin/Max`, `RotMin/Max` |
| move duration | 50–10000 ms | `STAGE_MOVE_DURATION_*` | `MoveDurationMin/Max` |
| set/tween duration | 0–10000 ms | `STAGE_SET_DURATION_MAX` | `SetDurationMax` |
| pulse duration | 50–2000 ms | `STAGE_PULSE_DURATION_*` | `PulseDurationMin/Max` |
| move intensity | 0–2 | `STAGE_MOVE_INTENSITY_MAX` | `MoveIntensityMax` |
| view slots | 1–9 | `STAGE_VIEW_SLOT_*` | `ViewSlotMin/Max` |
| move keys | ≤ 16 | `STAGE_MOVE_MAX_KEYS` | `MoveMaxKeys` |
| state_changed rate | ≤ 10 Hz | `STAGE_STATE_EVENT_MAX_HZ` | `StateEventMaxHz` |
| flashes | ≤ 3 / s | `STAGE_FLASH_MAX_PER_SEC` | `FlashMaxPerSec` |
| perf guard | 2 ms for 5 s | `STAGE_PERF_BUDGET_MS`, `STAGE_PERF_WINDOW_S` | `PerfBudgetMS`, `PerfWindowS` |

**Live-safe mode** (rig property, default on) caps rotation speed at 20°/s, zoom speed at 0.5/s and roll at ±8°, keeps overscan at least at the computed minimum so frame edges never show, and forces the flash limiter on. Any manual command (`camera_set`, `camera_move`, `view_apply`) pauses the auto camera for 10 s. The plugin never executes text from the network: payloads are validated and the command queue is bounded.

## 10. UX mapping (Stage controls and the dock)

| UX concept | Protocol |
|---|---|
| Stage source / Stage Depth filter | Not used. Stage = `stage_camera_rig` scene filter (zero render passes) + `stage_fx` filter + per-source `stage_depth_blur` |
| Plugin status, rigs and FX filters | `get_info` (`get_info.rigs`, `get_info.fx`) + `get_state` |
| Look preset | recipe (`recipe_list`, `recipe_apply`); user presets are user recipes |
| Camera preset 1–9 / pad | views: `view_save`, `view_apply`, `view_list`, `view_delete` |
| "Update from current camera" | `view_save` on the same slot |
| Rename a view / change its transition | `view_save{keep_pose:true}` on the same slot |
| Transition curve Cut / Smooth / Whip / Punch | `transition`: `cut`, `smooth`, `whip`, `punch` |
| Auto camera Off / Gentle / Talk-reactive / Music-reactive | `camera_auto` mode `off`, `drift`, `chatting`, `singing` |
| Intensity 0–100 (default 35) | `intensity` 0–1 (default 0.35) |
| Manual position Pan X/Y, Zoom, Roll, Home | `camera_set` (absolute), `camera_set{relative:true}` (nudge), `camera_reset` |
| Depth slider −100..+100 (0 = character) | `layer_set.depth` via `d = clamp(10^(−v/100), 0.1, 10)` (±100 = depth 10..0.1, section 3) |
| Overscan (default 8 %) | bg auto-oversize; recipe `overscan` 0.08 |
| Freeze | `freeze` |
| Soft focus / DoF, rack focus | `stage_depth_blur` filter per source + `dof_set` (animated `focus_depth`) |
| FX slots: Bloom, Vignette, Chromatic aberration, Film grain, Color grade LUT, Light leak | `fx_set.params`: `bloom`, `vignette*`, `chroma`, `grain*`, `lut`/`lut_amount`, `leak*` |
| Beat pulse / Pulse hotkey | `fx_pulse` (`flash`, `chroma_kick`, `vignette_pulse`), recipe `audio_react` |
| Scanlines, Sparkle overlay | not in protocol v1 |
| All FX off | `fx_set{preset:"neutral"}` |
| Performance guard message | `perf_warning` |
| PANIC | `panic` |
| "AI Live Studio: Stop speaking / Skip / Mute AI / Panic / Approve draft" OBS hotkeys | `hotkey_forward` `ai_stop` / `ai_skip` / `ai_mute` / `ai_panic` (+ `ai_approve`) |

Recommended ids for the nine standard looks: `clean`, `cozy_chat`, `talk_show`, `singing_ballad`, `singing_hype`, `gaming_facecam`, `late_night_lofi`, `spotlight`, `product_showcase`; plus `brb` and `karaoke_spotlight`.

## 11. Versioning

`get_info.protocol` is `1`. Additive changes (new optional request fields, new requests, new events, new enum values in responses) keep version 1 and are documented here first. Removing or changing the meaning of a field bumps the protocol. Clients must check `protocol` and ignore unknown event types.
