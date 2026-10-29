# Parallaxa — plugin UX spec (properties, hotkeys, Tools menu, locale keys)

Status: v2, reconciled with protocol v1 (2026-10-27). Protocol: [`docs/protocol/vendor-api.md`](../protocol/vendor-api.md) and `include/stage-protocol.h` win over this file. This file, `docs/protocol` and the plugin code are complete on their own for the plugin; the locale files are generated from section 8 (`tools/gen-locale.py`).

Conventions used below:
- **Settings key** is the `obs_data` key of the filter's own settings. **Units rule:** a settings key has the same name as a protocol parameter only when it stores the protocol's unit (`exposure` in EV, `grain_size`, the `lut` and `leak` ids, and the rig-written `auto_strength` 0..1). A value the panel shows as a percentage has its own key ending in `_pct`, stored as a double 0 to 100 (so protocol values round-trip without rounding), and is converted with the table in section 2.1. The vendor handler and the preset loader never copy `fx_set.params` or a preset's `params` into the settings by key (`obs_data_apply`, or `obs_data_set_double` with the param name); they go through that table. Keys that are not protocol names are suggestions; the implementation may rename them (keeping the `_pct` rule), but labels, ranges, defaults and behaviour are the UX contract.
- Per-item data (role, depth, rest pose) and saved views are **not** filter settings: they live in item and scene private settings (`stage.role`, `stage.depth`, `stage.rest`, `stage.views`). Property rows for them are generated and write through to those keys.
- Every property label has a tooltip key `<label key>.Desc` (section 8). Labels are sentence case; units are slider suffixes; amounts show 0 to 100 % and map to 0..1 (protocol value = shown value / 100).
- Locale strings use `%1`, `%2` placeholders that the plugin fills (OBS locale files have no interpolation).
- Filter display names are product names and stay in English in every language.

## 1. Parallaxa Camera Rig (`stage_camera_rig`, filter on a scene)

Added to a scene source. If it is added to anything else, the panel shows only the info line `Stage.Info.NotScene` and the filter does nothing. A second rig on the same scene shows `Stage.Info.SecondRig` and stays inactive.

### Properties: Parallaxa Camera Rig

| Label (EN) | Locale key | Settings key | Type and range | Default | Behaviour / protocol |
|---|---|---|---|---|---|
| Look | `Stage.Prop.Look` | `look` | list: built-in recipes, then `Stage.List.YourLooks` separator, then user recipes; " (modified)" suffix after manual edits | `cozy_chat` | Selecting applies it over 1 s (`recipe_apply`) |
| Apply look | `Stage.Button.ApplyLook` | `apply_look` | button | | Re-applies the selected Look |
| Save as look… | `Stage.Button.SaveAsLook` | `save_as_look` | button | | Shows New look name and Save look |
| New look name | `Stage.Prop.NewLookName` | `new_look_name` | text, 1 to 47 characters; hidden until Save as look… | empty | Turned into an id matching the protocol id pattern |
| Save look | `Stage.Button.SaveLook` | `save_look` | button; hidden until Save as look… | | Writes a user recipe to the plugin config folder; reports `Stage.Info.LookSaved` or an error |
| (status) | `Stage.Prop.Status` | `status_info` | info text | | Shows the latest `Stage.Info.*` message |
| Live-safe mode | `Stage.Prop.LiveSafe` | `live_safe` | bool | on | Caps 20°/s, 0.5x/s zoom, ±8° roll, overscan minimum, flash limit; off shows `Stage.LivesafeOffWarning` |
| Camera (group) | `Stage.Group.Camera` | `grp_camera` | group | expanded | |
| Camera view | `Stage.Prop.View` | `view_slot` | list 1 to 9, items "1 · Wide" or `Stage.List.ViewEmpty` | 1 | Selects the slot for the two buttons below |
| Go to view | `Stage.Button.GoToView` | `view_go` | button | | `view_apply`; empty slot → `Stage.Info.ViewEmpty` |
| Save camera to this view | `Stage.Button.SaveView` | `view_save_btn` | button | | `view_save` with name, transition, time → `Stage.Info.ViewSaved` |
| View name | `Stage.Prop.ViewName` | `view_name` | text, at most 47 Unicode characters | `Stage.List.ViewDefault` ("View 1") | Stored with the view |
| View transition | `Stage.Prop.ViewTransition` | `view_transition` | list `cut`, `smooth`, `whip`, `punch` | `smooth` | |
| Transition time | `Stage.Prop.TransitionTime` | `view_duration_ms` | int 0 to 10,000, suffix " ms" | 600 | Ignored for `cut` |
| Auto camera | `Stage.Prop.AutoCamera` | `auto_mode` | list `off`, `drift`, `chatting`, `singing` | `drift` | `camera_auto` |
| Motion intensity | `Stage.Prop.MotionIntensity` | `auto_intensity` | int 0 to 100, suffix " %" | 35 | `intensity` 0.35 |
| Freeze camera | `Stage.Prop.Freeze` | `freeze` | bool | off | `freeze` (not saved: always off after OBS starts) |
| Manual position (group, collapsed) | `Stage.Group.Manual` | `grp_manual` | group | collapsed | |
| Pan X | `Stage.Prop.PanX` | `cam_x` | float, ± canvas width, suffix " px" | 0 | `camera_set` (smoothed) |
| Pan Y | `Stage.Prop.PanY` | `cam_y` | float, ± canvas height, suffix " px" | 0 | `camera_set` |
| Zoom | `Stage.Prop.Zoom` | `cam_zoom` | float 0.5 to 4, step 0.01, suffix "x" | 1 | `camera_set` |
| Roll | `Stage.Prop.Roll` | `cam_rot` | float −45 to 45, suffix "°" | 0 | `camera_set` (Live-safe limits to ±8°) |
| Home | `Stage.Button.Home` | `cam_home` | button | | `camera_reset` |
| Layers (group) | `Stage.Group.Layers` | `grp_layers` | group | expanded | First row: `Stage.Info.LayersHelp` |
| Auto-assign roles | `Stage.Button.AutoAssign` | `auto_assign` | button | | Name guesses: `bg|background|背景` → bg, `vts|spout|avatar|model|vtube` → char, `fg|foreground|前景` → fg |
| Refresh layers | `Stage.Button.RefreshLayers` | `refresh_layers` | button | | Rebuilds the layer rows |
| (item name) | item name, not translated | `role_<item_id>` | list `Stage.List.Role.*` | role guess, else `none` | `layer_set` `role`; writes `stage.role` |
| Depth | `Stage.Prop.LayerDepth` | `depth_<item_id>` | int −100 to +100 (`STAGE_UI_DEPTH_MIN/MAX`); shown only for bg, char, fg | from depth: bg −48, char 0, fg +22 | `layer_set` `depth` = `clamp(10^(-v/100), 0.1, 10)`; writes `stage.depth` |
| Depth of field (checkable group) | `Stage.Group.Dof` | `dof_enabled` | checkable group | off | Mirrors `amount` > 0 (see "Depth of field state" below) |
| Focus | `Stage.Prop.DofFocus` | `dof_focus` | int −100 to +100 (same mapping as depth) | 0 | `dof_set` `focus_depth`; moving it turns Focus follows character off |
| Blur amount | `Stage.Prop.DofAmount` | `dof_amount` | int 0 to 100, suffix " %" | 30 | `dof_set` `amount` = value / 100 while the group is ticked; kept as the restore value while it is unticked |
| Focus follows character | `Stage.Prop.DofFollowChar` | `dof_follow_char` | bool | on | Panel-only (no protocol field): while on, the rig sets `focus_depth` to the char layer's depth whenever that depth changes |
| (depth blur status) | `Stage.Prop.DofStatus` | `dof_info` | info text | | `Stage.Info.DofLayers` or `Stage.Info.DofNoBlur` |
| Audio bounce (group, collapsed) | `Stage.Group.Audio` | `grp_audio` | group | collapsed | |
| Audio source | `Stage.Prop.AudioSource` | `audio_source` | list of audio sources by name, first item `Stage.List.None` | none | volmeter on that source |
| Bounce amount | `Stage.Prop.AudioAmount` | `audio_amount` | int 0 to 100, suffix " %" | 25 | recipe `audio_react` amount |
| Attack | `Stage.Prop.AudioAttack` | `audio_attack_ms` | int 1 to 500, suffix " ms" | 30 | |
| Release | `Stage.Prop.AudioRelease` | `audio_release_ms` | int 10 to 2,000, suffix " ms" | 250 | |
| Advanced (group, collapsed) | `Stage.Group.Advanced` | `grp_advanced` | group | collapsed | |
| Smoothing | `Stage.Prop.Smoothing` | `smoothing_ms` | int 0 to 1,000, suffix " ms" | 120 | Spring time constant |
| Overscan | `Stage.Prop.Overscan` | `overscan_pct` | int 0 to 25, suffix " %" | 8 | Background auto-oversize (recipe `overscan` 0.08); Live-safe keeps the computed minimum |
| Max zoom | `Stage.Prop.MaxZoom` | `max_zoom` | float 1 to 4, suffix "x" | 2 | Clamp for auto, moves and bounce (explicit `camera_set` still allows the protocol range) |
| Max pan | `Stage.Prop.MaxPan` | `max_pan_pct` | int 0 to 50, suffix " %" of canvas | 15 | Same clamp scope as Max zoom |
| Shake and drift | `Stage.Prop.MotionScale` | `motion_scale` | int 0 to 100, suffix " %" | 100 | Below 100 % shake stays under 3 Hz; 0 % turns shake and drift off |
| Pause auto after manual input | `Stage.Prop.ManualPause` | `manual_pause_s` | int 0 to 60, suffix " s" | 10 | |
| Bake rest pose | `Stage.Button.BakeRest` | `bake_rest` | button | | Restores rest poses, keeps the rig |
| Reset camera | `Stage.Button.ResetCamera` | `reset_camera` | button | | `camera_reset` + auto off |
| About | `Stage.Prop.About` | `about` | info text `Stage.Info.About` + URL buttons `Stage.Button.Docs`, `Stage.Button.ReportBug` | | |

First add: when the rig is created on a scene with no enrolled items, it runs Auto-assign roles; if at least one item got a role it applies `cozy_chat` and shows `Stage.Info.LookApplied`, otherwise `Stage.Info.NoRoles`. If the scene has no Parallaxa FX, it also shows `Stage.Info.NeedFx` once.

**Depth of field state.** The rig holds one `amount` and one `focus_depth` (protocol, vendor-api.md section 3). The panel controls are views of that state, so the panel, hotkeys, docks and recipes never disagree:
- `dof_enabled` is ticked exactly when `amount` > 0. Unticking sends `dof_set` `amount` 0 and keeps Blur amount as the restore value; ticking sends `amount` = Blur amount / 100. A `dof_set` from anywhere (dock, AI Live Studio, hotkey, `recipe_apply`, vendor client) with `amount` > 0 ticks the group and moves the Blur amount slider to that value; `amount` 0 unticks it and leaves the slider where it was. There is no "unticked but driving" state.
- A new rig starts with `amount` 0 (group unticked). The first-add Look `cozy_chat` sets `amount` 0.2, so after a first add that found roles the group is ticked at 20 %; this changes nothing on screen until a layer carries Parallaxa Depth Blur.
- Focus follows character (default on) makes the rig write `focus_depth` = the char layer's depth (1 when no layer has role `char`) whenever that depth changes. Any `focus_depth` write that differs from the char layer's depth by more than 0.01 (Focus slider, Rack focus to background, a dock or AI Live Studio rack focus, a vendor `dof_set`, a recipe) turns follow off and the checkbox unticks; a `focus_depth` equal to the char layer's depth (Rack focus to character, a recipe with focus 1 on a default scene) turns it back on. So rack focus always works, including with the default settings.
- Panic sets `amount` 0 (group unticked) and turns follow on.

## 2. Parallaxa FX (`stage_fx`, filter on a scene or a source)

Checkable groups hold an `*_enabled` flag, and an unticked group sends its parameters at their neutral values. All neutral = the filter skips itself. Percentage sliders use `_pct` keys (units rule above); section 2.1 maps every `fx_set` parameter to its settings key.

### Properties: Parallaxa FX

| Label (EN) | Locale key | Settings key | Type and range | Default | Behaviour / protocol |
|---|---|---|---|---|---|
| Effects look | `Stage.Prop.FxPreset` | `fx_preset` | list of FX presets (built-in, then user) | `cozy` | `fx_set` `preset`; " (modified)" after edits |
| Reset to look | `Stage.Button.FxReset` | `fx_reset` | button | | Restores the preset values |
| (status) | `Stage.Prop.Status` | `status_info` | info text | | Perf guard, GPU fallback, flash cap |
| Colour (group) | `Stage.Group.Colour` | `grp_colour` | group | expanded | |
| Exposure | `Stage.Prop.Exposure` | `exposure` | float −2 to 2, step 0.05, suffix " EV" | preset | Same unit as the protocol |
| Contrast | `Stage.Prop.Contrast` | `contrast_pct` | float −100 to 100, step 1, suffix " %" | preset | `contrast` × 100 |
| Saturation | `Stage.Prop.Saturation` | `saturation_pct` | float −100 to 100, step 1, suffix " %" | preset | `saturation` × 100 |
| Temperature | `Stage.Prop.Temperature` | `temperature_pct` | float −100 to 100, step 1, suffix " %" | preset | `temperature` × 100 |
| Tint | `Stage.Prop.Tint` | `tint_pct` | float −100 to 100, step 1, suffix " %" | preset | `tint` × 100 |
| Colour grade LUT (checkable group) | `Stage.Group.Lut` | `lut_enabled` | checkable group | preset | |
| LUT | `Stage.Prop.Lut` | `lut` | list of LUT ids (PNG 512 x 512 strips from the plugin and user LUT folders), first item `Stage.List.None` | preset | id only, never a path |
| Amount | `Stage.Prop.LutAmount` | `lut_amount_pct` | float 0 to 100, step 1, suffix " %" | preset | `lut_amount` × 100 |
| Open my LUT folder | `Stage.Button.OpenLutFolder` | `lut_folder` | URL button (file URL of the user folder) | | |
| Vignette (checkable group) | `Stage.Group.Vignette` | `vignette_enabled` | checkable group | preset | |
| Amount | `Stage.Prop.VignetteAmount` | `vignette_pct` | float 0 to 100, step 1, suffix " %" | preset | `vignette` × 100 |
| Softness | `Stage.Prop.VignetteSoft` | `vignette_soft_pct` | float 0 to 100, step 1, suffix " %" | 50 | `vignette_soft` × 100 |
| Roundness | `Stage.Prop.VignetteRound` | `vignette_round_pct` | float 0 to 100, step 1, suffix " %" | 100 | `vignette_round` × 100 |
| Bloom (checkable group) | `Stage.Group.Bloom` | `bloom_enabled` | checkable group | preset | First row `Stage.Info.BloomCost` |
| Amount | `Stage.Prop.BloomAmount` | `bloom_pct` | float 0 to 100, step 1, suffix " %" | preset | `bloom` × 100; > 0 enables the half-resolution passes |
| Threshold | `Stage.Prop.BloomThreshold` | `bloom_threshold_pct` | float 0 to 100, step 1, suffix " %" | 80 | `bloom_threshold` × 100 |
| Chromatic aberration (checkable group) | `Stage.Group.Chroma` | `chroma_enabled` | checkable group | preset | |
| Amount | `Stage.Prop.ChromaAmount` | `chroma_pct` | float 0 to 100, step 1, suffix " %" | preset | `chroma` × 100 |
| Film grain (checkable group) | `Stage.Group.Grain` | `grain_enabled` | checkable group | preset | |
| Amount | `Stage.Prop.GrainAmount` | `grain_pct` | float 0 to 100, step 1, suffix " %" | preset | `grain` × 100 |
| Size | `Stage.Prop.GrainSize` | `grain_size` | float 0.5 to 4, step 0.1, suffix "x" | 1 | Same unit as the protocol |
| Light leak (checkable group) | `Stage.Group.Leak` | `leak_enabled` | checkable group | preset | |
| Texture | `Stage.Prop.Leak` | `leak` | list of light-leak ids, first item `Stage.List.None` | None | id only; no leak texture ships in v1 content, so the list holds only user textures until some ship |
| Amount | `Stage.Prop.LeakAmount` | `leak_amount_pct` | float 0 to 100, step 1, suffix " %" | preset | `leak_amount` × 100 |
| Speed | `Stage.Prop.LeakSpeed` | `leak_speed_pct` | float 0 to 200, step 1, suffix " %" | 20 | `leak_speed` × 100 |
| Test pulse (group) | `Stage.Group.Pulse` | `grp_pulse` | group | expanded | |
| Flash | `Stage.Button.PulseFlash` | `pulse_flash` | button | | `fx_pulse` `flash` (flash cap applies) |
| Chroma kick | `Stage.Button.PulseChroma` | `pulse_chroma_kick` | button | | `fx_pulse` `chroma_kick` |
| Vignette pulse | `Stage.Button.PulseVignette` | `pulse_vignette` | button | | `fx_pulse` `vignette_pulse` |

Status messages: `Stage.PerfPaused` (the paused part named with `Stage.Fx.*`), `Stage.GpuFallback`, `Stage.FlashCap` (shown after a pulse was merged). `.cube` LUT import is not in v1; the LUT tooltip says "PNG strips only".

### 2.1 Protocol ↔ settings conversion (Parallaxa FX)

The one place where protocol units meet panel units. The FX filter keeps it as a single static table (`{param, settings_key, scale}`) used in both directions: `fx_set` params and FX preset files are converted into settings with `setting = param × scale`, and the shader uniforms and user preset files saved from the panel are computed with `param = setting / scale`. A param absent from `fx_set` keeps its current setting; a param absent from a preset file takes its neutral value converted the same way. Panel range = protocol range × scale for every numeric row, checked against `vendor-api.md` 4.7. Protocol v1 has no way to read FX parameter values remotely (the `fx` object of the state holds only `preset`), so remote UIs show the value they sent, else the built-in preset's value, else "—" (section 7).

| Protocol param | Protocol range | Settings key | Panel range | Scale |
|---|---|---|---|---|
| `exposure` | −2..2 EV | `exposure` | −2 to 2 EV | 1 (identity) |
| `contrast` | −1..1 | `contrast_pct` | −100 to 100 % | 100 |
| `saturation` | −1..1 | `saturation_pct` | −100 to 100 % | 100 |
| `temperature` | −1..1 | `temperature_pct` | −100 to 100 % | 100 |
| `tint` | −1..1 | `tint_pct` | −100 to 100 % | 100 |
| `vignette` | 0..1 | `vignette_pct` | 0 to 100 % | 100 |
| `vignette_soft` | 0..1 | `vignette_soft_pct` | 0 to 100 % | 100 |
| `vignette_round` | 0..1 | `vignette_round_pct` | 0 to 100 % | 100 |
| `chroma` | 0..1 | `chroma_pct` | 0 to 100 % | 100 |
| `grain` | 0..1 | `grain_pct` | 0 to 100 % | 100 |
| `grain_size` | 0.5..4 | `grain_size` | 0.5 to 4x | 1 (identity) |
| `lut` | id | `lut` | id | id (identity) |
| `lut_amount` | 0..1 | `lut_amount_pct` | 0 to 100 % | 100 |
| `leak` | id | `leak` | id | id (identity) |
| `leak_amount` | 0..1 | `leak_amount_pct` | 0 to 100 % | 100 |
| `leak_speed` | 0..2 | `leak_speed_pct` | 0 to 200 % | 100 |
| `bloom` | 0..1 | `bloom_pct` | 0 to 100 % | 100 |
| `bloom_threshold` | 0..1 | `bloom_threshold_pct` | 0 to 100 % | 100 |

The `*_enabled` flags are panel-only: `fx_set` or a preset that sets a group's amount above 0 ticks that group; an amount of 0 unticks it (same rule as the rig's depth of field).

## 3. Parallaxa Depth Blur (`stage_depth_blur`, filter on a layer source)

Added to a background, foreground or sky source. The rig writes `auto_strength` while its Depth of field group is on and the layer's role is `bg`, `fg` or `sky` (sky uses depth 10), and marks the filter as driven with `driven_by`; at strength 0 the filter skips itself (zero cost).

### Properties: Parallaxa Depth Blur

| Label (EN) | Locale key | Settings key | Type and range | Default | Behaviour / protocol |
|---|---|---|---|---|---|
| (status) | `Stage.Prop.Status` | `status_info` | info text | | `Stage.Info.DepthBlurDriven` or `Stage.Info.DepthBlurManual` |
| Quality | `Stage.Prop.BlurQuality` | `quality` | list `Stage.List.Quality.Half`, `Stage.List.Quality.Quarter` | Half | Blur resolution (1/2 or 1/4) |
| Passes | `Stage.Prop.BlurPasses` | `passes` | int 2 to 4 | 3 | Dual-Kawase down/up passes |
| Strength | `Stage.Prop.BlurStrength` | `strength_pct` | float 0 to 100, step 1, suffix " %" | 0 | Manual strength, used while no rig drives the layer (`strength_pct / 100`). Disabled (greyed) while `driven_by` is set |
| (no widget) | | `auto_strength` | double 0..1, never shown | 0 | Rig-written strength (`STAGE_DEPTH_BLUR_KEY_AUTO_STRENGTH`, protocol unit), used while `driven_by` is set. Only the rig writes it |
| (no widget) | | `driven_by` | string, never shown | empty | uuid of the scene whose rig drives this layer; empty = not driven. Only the rig writes it (rules below). A settings key of this filter, not a vendor protocol constant |

**Driven is a state, not a timer.** The protocol (`vendor-api.md` section 3) has the rig write `auto_strength` only on ticks where `focus_depth` or `amount` changed, so a steady depth of field (Cozy Chat at 20 %) writes once and then stays quiet for the rest of the stream. The filter therefore never guesses "driven" from how recent a write is; the rig states it in `driven_by`:
- **Start driving.** A rig drives a layer while all of these hold: the rig is enabled and active (not a second rig), `amount` > 0, the item's role is `bg`, `fg` or `sky`, and the item's source carries this filter. When that becomes true (amount rises above 0, the role changes, the rig is created, loaded or enabled, or the filter is found when the layer cache is rebuilt), the rig writes `driven_by` = its scene uuid and `auto_strength` in one `obs_source_update`. The rig also marks its layer cache dirty on each enrolled layer source's filter-add and filter-remove signals, so adding the filter while depth of field is on is picked up on the next tick.
- **Stop driving.** When it becomes false (amount 0, role changed, item removed, filter removed, rig disabled, removed or destroyed, OBS exit), the rig writes `driven_by` = "" and `auto_strength` = 0 in one update, but only if `driven_by` still holds its own scene uuid. A source shown in two rigged scenes is driven by the rig that started last; the other leaves it alone.
- **Filter side.** The filter renders with `auto_strength` while `driven_by` is non-empty, and with `strength_pct / 100` while it is empty; it never writes either key itself. Driven: Strength is disabled and the status line shows `Stage.Info.DepthBlurDriven`. Not driven: Strength is enabled and the status line shows `Stage.Info.DepthBlurManual`. A `strength_pct` change that arrives while driven (for example from another obs-websocket client) is stored and takes effect when driving stops, so a stopped rig never leaves a stale blur and never loses the manual value.
- **Panel refresh.** Rig writes never move the slider (OBS would rebuild the panel on every tick). When `driven_by` changes, which happens only on start or stop and never per tick, the filter calls `obs_source_update_properties` so an open panel greys or enables Strength at once.
- **Load.** `driven_by` is saved with the source like any setting. The filter clears it in its create callback, and each active rig sets it again on its first tick (a new rig treats its first tick as a change). A crash, or a rig deleted with the scene collection closed, therefore cannot leave a layer stuck as driven.

## 4. Tools menu

Items added with `obs_frontend_add_tools_menu_item` (no Qt). Results go to the OBS log and, where a filter exists, to its status line.

| Item (EN) | Locale key | Does |
|---|---|---|
| Parallaxa: Set up current scene | `Stage.Tools.SetupScene` | Adds Parallaxa Camera Rig and Parallaxa FX to the current program scene if missing, runs Auto-assign roles, applies Cozy Chat |
| Parallaxa: Restore all rest poses | `Stage.Tools.RestoreRest` | Every rig restores its items to their rest poses and turns auto camera off (use before uninstalling) |
| Parallaxa: Panic reset | `Stage.Tools.Panic` | `panic` on all rigs and FX filters |
| Parallaxa: Import sample scene | `Stage.Tools.ImportSample` | Adds the sample scene of `presets/samples/parallaxa_sample.json` to the current scene collection, with the images at absolute paths in the installed data folder (no Missing Files step). Names already in use get a number ("Background 2"), so an existing scene or source is never overwritten; new uuids, so it can be imported again. Does not switch the program scene (in studio mode it becomes the preview) |

## 5. Hotkeys

Frontend hotkeys, acting on the current program scene, unbound by default.

| Hotkey (EN) | Locale key | Does |
|---|---|---|
| Parallaxa: Camera view %1 (1 to 9) | `Stage.Hotkey.View` | `view_apply` slot %1 |
| Parallaxa: Next camera view | `Stage.Hotkey.ViewNext` | next saved slot |
| Parallaxa: Previous camera view | `Stage.Hotkey.ViewPrev` | previous saved slot |
| Parallaxa: Camera home | `Stage.Hotkey.Home` | `camera_reset` |
| Parallaxa: Toggle auto camera | `Stage.Hotkey.AutoToggle` | `off` ↔ last mode |
| Parallaxa: Next auto camera mode | `Stage.Hotkey.AutoNext` | cycle `off`, `drift`, `chatting`, `singing` |
| Parallaxa: Freeze camera | `Stage.Hotkey.Freeze` | `freeze` toggle |
| Parallaxa: Zoom punch-in (hold) | `Stage.Hotkey.PunchHold` | Plugin-internal, not a move: press sends `camera_set` `relative` zoom +0.18 (the `punch` move's peak) over 120 ms with `ease_out_back` and keeps it while held; release sends `camera_set` back to the target from before the press over 200 ms. Live-safe zoom speed caps apply |
| Parallaxa: Move – %1 | `Stage.Hotkey.Move` | `camera_move` (one hotkey per move; %1 = the move's localized name) |
| Parallaxa: Toggle bloom (effect 1) | `Stage.Hotkey.FxBloom` | bloom ↔ neutral |
| Parallaxa: Toggle vignette (effect 2) | `Stage.Hotkey.FxVignette` | vignette ↔ neutral |
| Parallaxa: Toggle colour grade (effect 3) | `Stage.Hotkey.FxLut` | LUT amount ↔ 0 |
| Parallaxa: Toggle light leak (effect 4) | `Stage.Hotkey.FxLeak` | leak amount ↔ 0 |
| Parallaxa: All effects off | `Stage.Hotkey.FxOff` | `fx_set` preset `neutral` |
| Parallaxa: Pulse (one beat) | `Stage.Hotkey.Pulse` | `fx_pulse` `vignette_pulse` |
| Parallaxa: Rack focus to character | `Stage.Hotkey.RackChar` | `dof_set` `focus_depth` = the char layer's depth (1 if none) over 1.2 s; turns Focus follows character back on. With `amount` 0 it also ticks Depth of field at the Blur amount slider value |
| Parallaxa: Rack focus to background | `Stage.Hotkey.RackBg` | `dof_set` `focus_depth` = the farthest bg layer's depth (3 if none) over 1.2 s; turns Focus follows character off. With `amount` 0 it also ticks Depth of field at the Blur amount slider value |
| Parallaxa: Panic reset | `Stage.Hotkey.Panic` | `panic` |
| AI Live Studio: Stop speaking | `Stage.Hotkey.AiStop` | `hotkey_forward` `ai_stop` |
| AI Live Studio: Skip | `Stage.Hotkey.AiSkip` | `hotkey_forward` `ai_skip` |
| AI Live Studio: Mute AI | `Stage.Hotkey.AiMute` | `hotkey_forward` `ai_mute` |
| AI Live Studio: Panic | `Stage.Hotkey.AiPanic` | `hotkey_forward` `ai_panic` |
| AI Live Studio: Approve draft | `Stage.Hotkey.AiApprove` | `hotkey_forward` `ai_approve` |

## 6. Status line catalogue

One info line per filter (`Stage.Prop.Status`) shows the latest message; errors stay until the cause is gone, other messages until the panel closes.

| When | Key |
|---|---|
| A Look was applied | `Stage.Info.LookApplied` |
| A Look was saved / name empty / name taken | `Stage.Info.LookSaved`, `Stage.Info.LookNameEmpty`, `Stage.Info.LookNameTaken` |
| No role could be guessed | `Stage.Info.NoRoles` |
| The scene has no Parallaxa FX | `Stage.Info.NeedFx` |
| Rig added to a non-scene / second rig | `Stage.Info.NotScene`, `Stage.Info.SecondRig` |
| View saved / empty view | `Stage.Info.ViewSaved`, `Stage.Info.ViewEmpty` |
| Live-safe turned off | `Stage.LivesafeOffWarning` |
| Shader failed to compile | `Stage.GpuFallback` |
| Performance guard paused a part | `Stage.PerfPaused` |
| A pulse was merged by the flash limit | `Stage.FlashCap` |
| Depth blur driven / manual | `Stage.Info.DepthBlurDriven`, `Stage.Info.DepthBlurManual` |

## 7. Standalone dock (`data/dock/dock-standalone.html`)

For Parallaxa users without AI Live Studio: the Stage controls (Looks, camera views, auto camera, moves, manual camera, layers and depth, depth of field, effects) without any AI feature, at most 32 KB of gzipped JS with its strings in all three languages inline (gated by `dock-src/src/dock/standalone.test.ts`), 300 to 420 px wide.
- Added through OBS > Docks > Custom Browser Docks with a file URL to the plugin's data folder; the plugin's About line links to the setup steps.
- First run asks for the obs-websocket host (default `ws://127.0.0.1:4455`) and password once, keeps them in that OBS browser profile, and offers "Forget password".
- It computes the obs-websocket auth hash with a bundled SHA-256 (a file page is not a secure context) and subscribes to vendor events (`Vendors` subscription).
- Connection states: OBS closed, WebSocket off or Safe Mode (`stage.obs_closed`), wrong password (`stage_sa.wrong`), Parallaxa not loaded (`stage.not_found`), scene without rig (`stage.no_rig`).
- Effects lists: protocol v1 cannot list FX presets, LUTs or light leaks remotely, so the dock offers only the built-in ids it ships with and leaves user presets, user LUTs and leak textures to the OBS panel. Effect sliders use the section 2.1 units and, because protocol v1 cannot report FX param values, show the value sent from the dock, else the built-in preset's value, else "—".

## 8. Locale keys (EN / JA / ID)

`data/locale/en-US.ini`, `ja-JP.ini`, `id-ID.ini` contain exactly these keys. `%1`, `%2` are filled by the plugin. Where a string also exists in the dock or in AI Live Studio (`stage.*` keys), the wording is the same.

| Key | English | 日本語 | Bahasa Indonesia |
|---|---|---|---|
| `Stage.Description` | Depth, parallax, virtual camera moves and visual effects for 2D VTubers. | 2D VTuber 向けの奥行き・パララックス・バーチャルカメラ演出・映像エフェクト。 | Kedalaman, paralaks, gerakan kamera virtual, dan efek visual untuk VTuber 2D. |
| `Stage.Filter.CameraRig` | Parallaxa Camera Rig | Parallaxa Camera Rig | Parallaxa Camera Rig |
| `Stage.Filter.FX` | Parallaxa FX | Parallaxa FX | Parallaxa FX |
| `Stage.Filter.DepthBlur` | Parallaxa Depth Blur | Parallaxa Depth Blur | Parallaxa Depth Blur |
| `Stage.Prop.Status` | Status | 状態 | Status |
| `Stage.Prop.Look` | Look | ルック | Tampilan |
| `Stage.Prop.Look.Desc` | A ready-made style: layer depths, auto camera, effects and depth of field in one click. | レイヤーの奥行き・オートカメラ・エフェクト・被写界深度をまとめて設定する既製のスタイルです。 | Gaya siap pakai: kedalaman layer, kamera otomatis, efek, dan kedalaman bidang dalam satu klik. |
| `Stage.Prop.NewLookName` | New look name | 新しいルックの名前 | Nama tampilan baru |
| `Stage.Prop.NewLookName.Desc` | Name for your own look. It appears under "Your looks" in every scene. | 自作ルックの名前です。すべてのシーンの「自分のルック」に表示されます。 | Nama untuk tampilan buatanmu. Muncul di "Tampilan milikmu" di setiap scene. |
| `Stage.Prop.LiveSafe` | Live-safe mode | ライブセーフモード | Mode aman live |
| `Stage.Prop.LiveSafe.Desc` | Limits camera speed and roll, keeps frame edges hidden and limits flashes. Leave on while streaming. | カメラの速度と傾きを制限し、画面端が見えないようにし、フラッシュを制限します。配信中はオンのままにしてください。 | Membatasi kecepatan dan kemiringan kamera, menyembunyikan tepi gambar, dan membatasi kilatan. Biarkan aktif saat siaran. |
| `Stage.Group.Camera` | Camera | カメラ | Kamera |
| `Stage.Prop.View` | Camera view | カメラビュー | Tampilan kamera |
| `Stage.Prop.View.Desc` | One of nine saved camera positions for this scene. | このシーンに保存された9つのカメラ位置のひとつです。 | Salah satu dari sembilan posisi kamera tersimpan untuk scene ini. |
| `Stage.Prop.ViewName` | View name | ビュー名 | Nama tampilan |
| `Stage.Prop.ViewName.Desc` | Shown on the dock's camera pad. Up to 47 characters. | ドックのカメラパッドに表示されます。47文字まで。 | Ditampilkan di pad kamera dock. Maksimal 47 karakter. |
| `Stage.Prop.ViewTransition` | View transition | ビューの切り替え | Transisi tampilan |
| `Stage.Prop.ViewTransition.Desc` | How the camera travels to this view: cut, smooth, whip or punch. | このビューへのカメラの移り方です：カット、なめらか、ウィップ、パンチ。 | Cara kamera berpindah ke tampilan ini: potong, halus, sentak, atau hentak. |
| `Stage.Prop.TransitionTime` | Transition time | 切り替え時間 | Waktu transisi |
| `Stage.Prop.TransitionTime.Desc` | How long the move to this view takes. | このビューへの移動にかかる時間です。 | Lama perpindahan ke tampilan ini. |
| `Stage.Prop.AutoCamera` | Auto camera | オートカメラ | Kamera otomatis |
| `Stage.Prop.AutoCamera.Desc` | Gentle drift moves slowly on its own; Talk-reactive follows speech; Music-reactive follows the beat. | ゆったりは自然にゆっくり動き、トーク連動は話し声に、音楽連動はビートに合わせて動きます。 | Gerak halus bergerak pelan sendiri; Ikut bicara mengikuti suara; Ikut musik mengikuti irama. |
| `Stage.Prop.MotionIntensity` | Motion intensity | 動きの強さ | Intensitas gerak |
| `Stage.Prop.MotionIntensity.Desc` | How much the auto camera moves. 35 % is subtle. | オートカメラの動きの大きさです。35%で控えめです。 | Seberapa besar kamera otomatis bergerak. 35% terasa halus. |
| `Stage.Prop.Freeze` | Freeze camera | カメラを固定 | Bekukan kamera |
| `Stage.Prop.Freeze.Desc` | Holds the camera still: auto camera, bounce and moves stop until you turn this off. | カメラを止めます。オフにするまでオートカメラ・バウンス・ムーブは停止します。 | Menahan kamera: kamera otomatis, pantulan, dan gerakan berhenti sampai kamu mematikannya. |
| `Stage.Group.Manual` | Manual position | 手動位置 | Posisi manual |
| `Stage.Prop.PanX` | Pan X | パン X | Geser X |
| `Stage.Prop.PanX.Desc` | Moves the view left or right. | 画面を左右に動かします。 | Menggeser tampilan ke kiri atau kanan. |
| `Stage.Prop.PanY` | Pan Y | パン Y | Geser Y |
| `Stage.Prop.PanY.Desc` | Moves the view up or down. | 画面を上下に動かします。 | Menggeser tampilan ke atas atau bawah. |
| `Stage.Prop.Zoom` | Zoom | ズーム | Zoom |
| `Stage.Prop.Zoom.Desc` | 1x is the normal framing; higher zooms in. | 1xが通常の画角で、大きくするとズームインします。 | 1x adalah bingkai normal; lebih besar berarti memperbesar. |
| `Stage.Prop.Roll` | Roll | 傾き | Kemiringan |
| `Stage.Prop.Roll.Desc` | Tilts the view. Live-safe limits it to 8°. | 画面を傾けます。ライブセーフでは8°までです。 | Memiringkan tampilan. Mode aman live membatasinya 8°. |
| `Stage.Group.Layers` | Layers | レイヤー | Layer |
| `Stage.Prop.LayerDepth` | Depth | 奥行き | Kedalaman |
| `Stage.Prop.LayerDepth.Desc` | 0 = character plane. Below 0 is farther and moves less; above 0 is nearer and moves more. | 0がキャラクターの面です。0より小さいと遠くなり動きが小さく、大きいと近くなり動きが大きくなります。 | 0 = bidang karakter. Di bawah 0 lebih jauh dan bergerak lebih sedikit; di atas 0 lebih dekat dan bergerak lebih banyak. |
| `Stage.Group.Dof` | Depth of field | 被写界深度 | Kedalaman bidang |
| `Stage.Prop.DofFocus` | Focus | フォーカス | Fokus |
| `Stage.Prop.DofFocus.Desc` | The depth that stays sharp. 0 = the character. Moving it turns off Focus follows character. | ピントが合う奥行きです。0がキャラクターです。動かすと「キャラクターにピントを合わせる」がオフになります。 | Kedalaman yang tetap tajam. 0 = karakter. Menggesernya mematikan Fokus mengikuti karakter. |
| `Stage.Prop.DofAmount` | Blur amount | ぼかしの量 | Jumlah blur |
| `Stage.Prop.DofAmount.Desc` | How strongly layers away from the focus are blurred. | フォーカスから離れたレイヤーをどれだけぼかすかです。 | Seberapa kuat layer yang jauh dari fokus dibuat blur. |
| `Stage.Prop.DofFollowChar` | Focus follows character | キャラクターにピントを合わせる | Fokus mengikuti karakter |
| `Stage.Prop.DofFollowChar.Desc` | Keeps the character layer sharp, whatever its depth. | キャラクターのレイヤーを常にくっきり保ちます。 | Menjaga layer karakter tetap tajam, berapa pun kedalamannya. |
| `Stage.Prop.DofStatus` | Depth blur layers | ぼかしレイヤー | Layer blur |
| `Stage.Group.Audio` | Audio bounce | オーディオバウンス | Pantulan audio |
| `Stage.Prop.AudioSource` | Audio source | 音声ソース | Sumber audio |
| `Stage.Prop.AudioSource.Desc` | The camera bounces with this source's volume, for example your mic or music. | このソースの音量に合わせてカメラが弾みます（マイクや音楽など）。 | Kamera memantul mengikuti volume sumber ini, misalnya mik atau musik. |
| `Stage.Prop.AudioAmount` | Bounce amount | バウンスの量 | Jumlah pantulan |
| `Stage.Prop.AudioAmount.Desc` | How much the camera zooms with loud sounds. | 大きな音でどれだけズームするかです。 | Seberapa banyak kamera memperbesar saat suara keras. |
| `Stage.Prop.AudioAttack` | Attack | アタック | Attack |
| `Stage.Prop.AudioAttack.Desc` | How fast the bounce reacts to a sound. | 音にどれだけ速く反応するかです。 | Seberapa cepat pantulan bereaksi terhadap suara. |
| `Stage.Prop.AudioRelease` | Release | リリース | Release |
| `Stage.Prop.AudioRelease.Desc` | How fast the bounce settles after a sound. | 音のあとどれだけ速く元に戻るかです。 | Seberapa cepat pantulan kembali tenang setelah suara. |
| `Stage.Group.Advanced` | Advanced | 詳細設定 | Lanjutan |
| `Stage.Prop.Smoothing` | Smoothing | スムージング | Penghalusan |
| `Stage.Prop.Smoothing.Desc` | Softens every change so sliders never jump on stream. | すべての変化をなめらかにし、配信中にスライダーで画面が飛ばないようにします。 | Menghaluskan setiap perubahan agar slider tidak membuat lompatan di siaran. |
| `Stage.Prop.Overscan` | Overscan | オーバースキャン | Overscan |
| `Stage.Prop.Overscan.Desc` | Enlarges background layers so their edges never show while the camera moves. | カメラが動いても端が見えないよう、背景レイヤーを拡大します。 | Memperbesar layer latar belakang agar tepinya tidak terlihat saat kamera bergerak. |
| `Stage.Prop.MaxZoom` | Max zoom | 最大ズーム | Zoom maksimal |
| `Stage.Prop.MaxZoom.Desc` | Upper limit for auto camera, moves and bounce. | オートカメラ・ムーブ・バウンスの上限です。 | Batas atas untuk kamera otomatis, gerakan, dan pantulan. |
| `Stage.Prop.MaxPan` | Max pan | 最大パン | Geser maksimal |
| `Stage.Prop.MaxPan.Desc` | How far auto camera, moves and bounce may pan, as a share of the canvas. | オートカメラ・ムーブ・バウンスで動ける範囲（キャンバスに対する割合）です。 | Seberapa jauh kamera otomatis, gerakan, dan pantulan boleh menggeser, sebagai bagian dari kanvas. |
| `Stage.Prop.MotionScale` | Shake and drift | 揺れと漂い | Guncangan dan gerak halus |
| `Stage.Prop.MotionScale.Desc` | Scales shake and drift. Below 100 % shake stays slow; 0 % turns both off for motion-sensitive viewers. | 揺れと漂いの量です。100%未満では揺れがゆっくりになり、0%で両方オフになります（動きに敏感な視聴者向け）。 | Mengatur guncangan dan gerak halus. Di bawah 100% guncangan tetap pelan; 0% mematikan keduanya untuk penonton yang sensitif gerakan. |
| `Stage.Prop.ManualPause` | Pause auto after manual input | 手動操作後にオートを一時停止 | Jeda otomatis setelah input manual |
| `Stage.Prop.ManualPause.Desc` | After you move the camera yourself, auto camera waits this long before blending back in. | 手動でカメラを動かしたあと、オートカメラが戻るまでの待ち時間です。 | Setelah kamu menggerakkan kamera sendiri, kamera otomatis menunggu selama ini sebelum kembali. |
| `Stage.Prop.About` | About | 情報 | Tentang |
| `Stage.Prop.FxPreset` | Effects look | エフェクトのルック | Tampilan efek |
| `Stage.Prop.FxPreset.Desc` | A ready-made set of colour and effect values. Adjust below after picking one. | 色とエフェクトの既製セットです。選んでから下で調整できます。 | Set warna dan efek siap pakai. Sesuaikan di bawah setelah memilih. |
| `Stage.Group.Colour` | Colour | 色 | Warna |
| `Stage.Prop.Exposure` | Exposure | 露出 | Eksposur |
| `Stage.Prop.Exposure.Desc` | Brightens or darkens the whole picture. | 画面全体を明るくまたは暗くします。 | Mencerahkan atau menggelapkan seluruh gambar. |
| `Stage.Prop.Contrast` | Contrast | コントラスト | Kontras |
| `Stage.Prop.Contrast.Desc` | Difference between dark and bright areas. | 暗い部分と明るい部分の差です。 | Perbedaan antara area gelap dan terang. |
| `Stage.Prop.Saturation` | Saturation | 彩度 | Saturasi |
| `Stage.Prop.Saturation.Desc` | How vivid the colours are. | 色の鮮やかさです。 | Seberapa cerah warnanya. |
| `Stage.Prop.Temperature` | Temperature | 色温度 | Suhu warna |
| `Stage.Prop.Temperature.Desc` | Warmer (orange) or cooler (blue) picture. | 暖かい（オレンジ）か冷たい（青）色味にします。 | Gambar lebih hangat (oranye) atau lebih dingin (biru). |
| `Stage.Prop.Tint` | Tint | 色かぶり | Rona |
| `Stage.Prop.Tint.Desc` | Shifts colours toward green or magenta. | 色味を緑またはマゼンタ寄りにします。 | Menggeser warna ke hijau atau magenta. |
| `Stage.Group.Lut` | Colour grade LUT | カラーグレーディングLUT | LUT color grading |
| `Stage.Prop.Lut` | LUT | LUT | LUT |
| `Stage.Prop.Lut.Desc` | A colour grade file (PNG strips only). Put your own in the LUT folder. | カラーグレードのファイルです（PNGストリップのみ）。自作のものはLUTフォルダに入れてください。 | File color grading (hanya strip PNG). Taruh milikmu di folder LUT. |
| `Stage.Prop.LutAmount` | Amount | 量 | Jumlah |
| `Stage.Prop.LutAmount.Desc` | How strongly the LUT is mixed in. | LUTをどれだけ混ぜるかです。 | Seberapa kuat LUT dicampurkan. |
| `Stage.Group.Vignette` | Vignette | ビネット | Vinyet |
| `Stage.Prop.VignetteAmount` | Amount | 量 | Jumlah |
| `Stage.Prop.VignetteAmount.Desc` | Darkens the corners to draw the eye to the centre. | 四隅を暗くして中央に視線を集めます。 | Menggelapkan sudut agar mata tertuju ke tengah. |
| `Stage.Prop.VignetteSoft` | Softness | やわらかさ | Kelembutan |
| `Stage.Prop.VignetteSoft.Desc` | How gradually the corners darken. | 四隅の暗さのなだらかさです。 | Seberapa bertahap sudut menjadi gelap. |
| `Stage.Prop.VignetteRound` | Roundness | 丸み | Kebulatan |
| `Stage.Prop.VignetteRound.Desc` | 100 % is a circle; lower follows the frame shape. | 100%で円形、低いほど画面の形に沿います。 | 100% berbentuk lingkaran; lebih rendah mengikuti bentuk bingkai. |
| `Stage.Group.Bloom` | Bloom | ブルーム | Bloom |
| `Stage.Prop.BloomAmount` | Amount | 量 | Jumlah |
| `Stage.Prop.BloomAmount.Desc` | Soft glow around bright areas. | 明るい部分のまわりにやわらかな光を加えます。 | Cahaya lembut di sekitar area terang. |
| `Stage.Prop.BloomThreshold` | Threshold | しきい値 | Ambang |
| `Stage.Prop.BloomThreshold.Desc` | Only areas brighter than this glow. | これより明るい部分だけが光ります。 | Hanya area yang lebih terang dari ini yang bercahaya. |
| `Stage.Group.Chroma` | Chromatic aberration | 色収差 | Aberasi kromatik |
| `Stage.Prop.ChromaAmount` | Amount | 量 | Jumlah |
| `Stage.Prop.ChromaAmount.Desc` | Splits colours slightly at the edges for a lens look. | 端で色をわずかにずらし、レンズらしさを出します。 | Memisahkan warna sedikit di tepi untuk kesan lensa. |
| `Stage.Group.Grain` | Film grain | フィルムグレイン | Butiran film |
| `Stage.Prop.GrainAmount` | Amount | 量 | Jumlah |
| `Stage.Prop.GrainAmount.Desc` | Adds moving film grain. | 動くフィルムの粒子を加えます。 | Menambahkan butiran film yang bergerak. |
| `Stage.Prop.GrainSize` | Size | サイズ | Ukuran |
| `Stage.Prop.GrainSize.Desc` | Size of each grain. | 粒子の大きさです。 | Ukuran tiap butiran. |
| `Stage.Group.Leak` | Light leak | 光漏れ | Kebocoran cahaya |
| `Stage.Prop.Leak` | Texture | テクスチャ | Tekstur |
| `Stage.Prop.Leak.Desc` | The light-leak image that drifts over the picture. | 画面の上を流れる光漏れの画像です。 | Gambar kebocoran cahaya yang melayang di atas gambar. |
| `Stage.Prop.LeakAmount` | Amount | 量 | Jumlah |
| `Stage.Prop.LeakAmount.Desc` | How visible the light leak is. | 光漏れの見え方の強さです。 | Seberapa terlihat kebocoran cahaya. |
| `Stage.Prop.LeakSpeed` | Speed | 速さ | Kecepatan |
| `Stage.Prop.LeakSpeed.Desc` | How fast the light leak drifts. | 光漏れが流れる速さです。 | Seberapa cepat kebocoran cahaya bergerak. |
| `Stage.Group.Pulse` | Test pulse | テストパルス | Uji denyut |
| `Stage.Prop.BlurQuality` | Quality | 品質 | Kualitas |
| `Stage.Prop.BlurQuality.Desc` | Quarter resolution is faster and softer; half is sharper. | 1/4解像度は速くてやわらかく、1/2はよりくっきりします。 | Resolusi seperempat lebih cepat dan lembut; setengah lebih tajam. |
| `Stage.Prop.BlurPasses` | Passes | パス数 | Jumlah pass |
| `Stage.Prop.BlurPasses.Desc` | More passes allow a wider blur at a small extra cost. | パスを増やすとわずかな負荷でより広くぼかせます。 | Lebih banyak pass memungkinkan blur lebih lebar dengan sedikit biaya tambahan. |
| `Stage.Prop.BlurStrength` | Strength | 強さ | Kekuatan |
| `Stage.Prop.BlurStrength.Desc` | Set by the Parallaxa Camera Rig while depth of field is on; set it yourself otherwise. | 被写界深度がオンの間はParallaxa Camera Rigが設定します。それ以外は手動で設定します。 | Diatur oleh Parallaxa Camera Rig saat kedalaman bidang aktif; selain itu atur sendiri. |
| `Stage.List.YourLooks` | — Your looks — | — 自分のルック — | — Tampilan milikmu — |
| `Stage.List.Modified` | %1 (modified) | %1（変更あり） | %1 (diubah) |
| `Stage.List.ViewDefault` | View %1 | ビュー%1 | Tampilan %1 |
| `Stage.List.ViewEmpty` | %1 · (empty) | %1 ·（空） | %1 · (kosong) |
| `Stage.List.None` | None | なし | Tidak ada |
| `Stage.List.Auto.Off` | Off | オフ | Mati |
| `Stage.List.Auto.Drift` | Gentle drift | ゆったり | Gerak halus |
| `Stage.List.Auto.Chatting` | Talk-reactive | トーク連動 | Ikut bicara |
| `Stage.List.Auto.Singing` | Music-reactive | 音楽連動 | Ikut musik |
| `Stage.List.Transition.Cut` | Cut | カット | Potong |
| `Stage.List.Transition.Smooth` | Smooth | なめらか | Halus |
| `Stage.List.Transition.Whip` | Whip | ウィップ | Sentak |
| `Stage.List.Transition.Punch` | Punch | パンチ | Hentak |
| `Stage.List.Role.None` | Not moved | 動かさない | Tidak digerakkan |
| `Stage.List.Role.Bg` | Background | 背景 | Latar belakang |
| `Stage.List.Role.Char` | Character | キャラクター | Karakter |
| `Stage.List.Role.Fg` | Foreground | 前景 | Latar depan |
| `Stage.List.Role.Sky` | Sky (very far) | 空（とても遠く） | Langit (sangat jauh) |
| `Stage.List.Role.Static` | Pinned to screen | 画面に固定 | Disematkan ke layar |
| `Stage.List.Quality.Half` | Half resolution | 1/2解像度 | Resolusi setengah |
| `Stage.List.Quality.Quarter` | Quarter resolution | 1/4解像度 | Resolusi seperempat |
| `Stage.Button.ApplyLook` | Apply look | ルックを適用 | Terapkan tampilan |
| `Stage.Button.SaveAsLook` | Save as look… | ルックとして保存… | Simpan sebagai tampilan… |
| `Stage.Button.SaveLook` | Save look | ルックを保存 | Simpan tampilan |
| `Stage.Button.GoToView` | Go to view | ビューへ移動 | Ke tampilan |
| `Stage.Button.SaveView` | Save camera to this view | このビューにカメラを保存 | Simpan kamera ke tampilan ini |
| `Stage.Button.Home` | Home | ホーム | Posisi awal |
| `Stage.Button.AutoAssign` | Auto-assign roles | 役割を自動設定 | Atur peran otomatis |
| `Stage.Button.RefreshLayers` | Refresh layers | レイヤーを更新 | Muat ulang layer |
| `Stage.Button.BakeRest` | Bake rest pose | 基本位置に戻す | Kembalikan posisi dasar |
| `Stage.Button.ResetCamera` | Reset camera | カメラをリセット | Reset kamera |
| `Stage.Button.Docs` | Docs | ドキュメント | Dokumentasi |
| `Stage.Button.ReportBug` | Report a bug | 不具合を報告 | Laporkan bug |
| `Stage.Button.FxReset` | Reset to look | ルックに戻す | Kembalikan ke tampilan |
| `Stage.Button.OpenLutFolder` | Open my LUT folder | LUTフォルダを開く | Buka folder LUT-ku |
| `Stage.Button.PulseFlash` | Flash | フラッシュ | Kilat |
| `Stage.Button.PulseChroma` | Chroma kick | 色ずれキック | Hentakan kroma |
| `Stage.Button.PulseVignette` | Vignette pulse | ビネットパルス | Denyut vinyet |
| `Stage.Info.About` | Parallaxa %1 · protocol %2 | Parallaxa %1 · プロトコル %2 | Parallaxa %1 · protokol %2 |
| `Stage.Info.LookApplied` | Applied "%1" to %2 layers. | 「%1」を%2個のレイヤーに適用しました。 | "%1" diterapkan ke %2 layer. |
| `Stage.Info.LookSaved` | Saved "%1". | 「%1」を保存しました。 | "%1" tersimpan. |
| `Stage.Info.LookNameEmpty` | Type a name for the look. | ルックの名前を入力してください。 | Ketik nama untuk tampilan ini. |
| `Stage.Info.LookNameTaken` | A look named "%1" already exists. | 「%1」という名前のルックはすでにあります。 | Tampilan bernama "%1" sudah ada. |
| `Stage.Info.NoRoles` | Pick a role for at least one layer below. | 下のレイヤーのうち少なくとも1つに役割を設定してください。 | Pilih peran untuk setidaknya satu layer di bawah. |
| `Stage.Info.NeedFx` | Add Parallaxa FX to this scene to get the look's colours and effects. | ルックの色とエフェクトを使うには、このシーンにParallaxa FXを追加してください。 | Tambahkan Parallaxa FX ke scene ini untuk mendapatkan warna dan efek dari tampilan. |
| `Stage.Info.NotScene` | Add Parallaxa Camera Rig to a scene, not to a source. | Parallaxa Camera Rigはソースではなくシーンに追加してください。 | Tambahkan Parallaxa Camera Rig ke scene, bukan ke source. |
| `Stage.Info.SecondRig` | This scene already has a Parallaxa Camera Rig. This one stays off. | このシーンにはすでにParallaxa Camera Rigがあります。こちらは無効のままです。 | Scene ini sudah punya Parallaxa Camera Rig. Yang ini tetap nonaktif. |
| `Stage.Info.LayersHelp` | Pick a role for each layer. Depth 0 = character plane, below 0 = farther, above 0 = nearer. | 各レイヤーの役割を選んでください。奥行き0がキャラクターの面、0未満は遠く、0より上は近くです。 | Pilih peran untuk setiap layer. Kedalaman 0 = bidang karakter, di bawah 0 = lebih jauh, di atas 0 = lebih dekat. |
| `Stage.Info.DofLayers` | %1 layers have Parallaxa Depth Blur. | %1個のレイヤーにParallaxa Depth Blurがあります。 | %1 layer memiliki Parallaxa Depth Blur. |
| `Stage.Info.DofNoBlur` | Add Parallaxa Depth Blur to a layer (its Filters) to blur it. | ぼかしたいレイヤーのフィルタにParallaxa Depth Blurを追加してください。 | Tambahkan Parallaxa Depth Blur ke layer (di Filter-nya) untuk membuatnya blur. |
| `Stage.Info.ViewSaved` | Saved the camera to view %1. | カメラをビュー%1に保存しました。 | Kamera disimpan ke tampilan %1. |
| `Stage.Info.ViewEmpty` | View %1 is empty. Frame a shot, then click Save camera to this view. | ビュー%1は空です。画角を決めてから「このビューにカメラを保存」をクリックしてください。 | Tampilan %1 kosong. Atur bingkai, lalu klik Simpan kamera ke tampilan ini. |
| `Stage.Info.BloomCost` | Bloom adds two half-resolution passes. | ブルームは1/2解像度のパスを2回追加します。 | Bloom menambah dua pass resolusi setengah. |
| `Stage.Info.DepthBlurDriven` | Blur follows this layer's distance from the focus (set in Parallaxa Camera Rig). | ぼかしはフォーカスからのこのレイヤーの距離に応じて変わります（Parallaxa Camera Rigで設定）。 | Blur mengikuti jarak layer ini dari fokus (diatur di Parallaxa Camera Rig). |
| `Stage.Info.DepthBlurManual` | No Parallaxa Camera Rig drives this layer. Set the strength yourself. | このレイヤーを制御するParallaxa Camera Rigがありません。強さを手動で設定してください。 | Tidak ada Parallaxa Camera Rig yang mengatur layer ini. Atur kekuatannya sendiri. |
| `Stage.LivesafeOffWarning` | Live-safe is off. Big moves will show on stream. | ライブセーフがオフです。大きな動きがそのまま配信に映ります。 | Mode aman live mati. Gerakan besar akan terlihat di siaran. |
| `Stage.GpuFallback` | Effects unavailable on this GPU — showing the scene without effects. | このGPUではエフェクトを使用できません。エフェクトなしで表示しています。 | Efek tidak tersedia di GPU ini — scene ditampilkan tanpa efek. |
| `Stage.PerfPaused` | %1 paused to keep the stream smooth. | 配信を安定させるため%1を一時停止しました。 | %1 dijeda agar siaran tetap lancar. |
| `Stage.FlashCap` | Flashes are limited to 3 per second. | フラッシュは1秒に3回までに制限されています。 | Kilatan dibatasi 3 kali per detik. |
| `Stage.Fx.Bloom` | Bloom | ブルーム | Bloom |
| `Stage.Fx.Lut` | Colour grade LUT | カラーグレーディングLUT | LUT color grading |
| `Stage.Fx.Leak` | Light leak | 光漏れ | Kebocoran cahaya |
| `Stage.Fx.Grain` | Film grain | フィルムグレイン | Butiran film |
| `Stage.Fx.Chroma` | Chromatic aberration | 色収差 | Aberasi kromatik |
| `Stage.Tools.SetupScene` | Parallaxa: Set up current scene | Parallaxa: 現在のシーンをセットアップ | Parallaxa: Siapkan scene saat ini |
| `Stage.Tools.RestoreRest` | Parallaxa: Restore all rest poses | Parallaxa: すべての基本位置に戻す | Parallaxa: Kembalikan semua posisi dasar |
| `Stage.Tools.Panic` | Parallaxa: Panic reset | Parallaxa: 緊急リセット | Parallaxa: Reset darurat |
| `Stage.Tools.ImportSample` | Parallaxa: Import sample scene | Parallaxa: サンプルシーンをインポート | Parallaxa: Impor scene contoh |
| `Stage.Hotkey.View` | Parallaxa: Camera view %1 | Parallaxa: カメラビュー%1 | Parallaxa: Tampilan kamera %1 |
| `Stage.Hotkey.ViewNext` | Parallaxa: Next camera view | Parallaxa: 次のカメラビュー | Parallaxa: Tampilan kamera berikutnya |
| `Stage.Hotkey.ViewPrev` | Parallaxa: Previous camera view | Parallaxa: 前のカメラビュー | Parallaxa: Tampilan kamera sebelumnya |
| `Stage.Hotkey.Home` | Parallaxa: Camera home | Parallaxa: カメラをホームへ | Parallaxa: Kamera ke posisi awal |
| `Stage.Hotkey.AutoToggle` | Parallaxa: Toggle auto camera | Parallaxa: オートカメラ切り替え | Parallaxa: Nyala/matikan kamera otomatis |
| `Stage.Hotkey.AutoNext` | Parallaxa: Next auto camera mode | Parallaxa: 次のオートカメラモード | Parallaxa: Mode kamera otomatis berikutnya |
| `Stage.Hotkey.Freeze` | Parallaxa: Freeze camera | Parallaxa: カメラを固定 | Parallaxa: Bekukan kamera |
| `Stage.Hotkey.PunchHold` | Parallaxa: Zoom punch-in (hold) | Parallaxa: パンチズーム（長押し） | Parallaxa: Zoom hentak (tahan) |
| `Stage.Hotkey.Move` | Parallaxa: Move – %1 | Parallaxa: ムーブ – %1 | Parallaxa: Gerakan – %1 |
| `Stage.Hotkey.FxBloom` | Parallaxa: Toggle bloom (effect 1) | Parallaxa: ブルーム切り替え（エフェクト1） | Parallaxa: Nyala/matikan bloom (efek 1) |
| `Stage.Hotkey.FxVignette` | Parallaxa: Toggle vignette (effect 2) | Parallaxa: ビネット切り替え（エフェクト2） | Parallaxa: Nyala/matikan vinyet (efek 2) |
| `Stage.Hotkey.FxLut` | Parallaxa: Toggle colour grade (effect 3) | Parallaxa: カラーグレード切り替え（エフェクト3） | Parallaxa: Nyala/matikan color grading (efek 3) |
| `Stage.Hotkey.FxLeak` | Parallaxa: Toggle light leak (effect 4) | Parallaxa: 光漏れ切り替え（エフェクト4） | Parallaxa: Nyala/matikan kebocoran cahaya (efek 4) |
| `Stage.Hotkey.FxOff` | Parallaxa: All effects off | Parallaxa: エフェクトをすべてオフ | Parallaxa: Matikan semua efek |
| `Stage.Hotkey.Pulse` | Parallaxa: Pulse (one beat) | Parallaxa: パルス（1拍） | Parallaxa: Denyut (satu ketukan) |
| `Stage.Hotkey.RackChar` | Parallaxa: Rack focus to character | Parallaxa: キャラクターにピント送り | Parallaxa: Pindah fokus ke karakter |
| `Stage.Hotkey.RackBg` | Parallaxa: Rack focus to background | Parallaxa: 背景にピント送り | Parallaxa: Pindah fokus ke latar belakang |
| `Stage.Hotkey.Panic` | Parallaxa: Panic reset | Parallaxa: 緊急リセット | Parallaxa: Reset darurat |
| `Stage.Hotkey.AiStop` | AI Live Studio: Stop speaking | AI Live Studio: 発話を停止 | AI Live Studio: Hentikan bicara |
| `Stage.Hotkey.AiSkip` | AI Live Studio: Skip | AI Live Studio: スキップ | AI Live Studio: Lewati |
| `Stage.Hotkey.AiMute` | AI Live Studio: Mute AI | AI Live Studio: AIをミュート | AI Live Studio: Bisukan AI |
| `Stage.Hotkey.AiPanic` | AI Live Studio: Panic | AI Live Studio: 緊急停止 | AI Live Studio: Darurat |
| `Stage.Hotkey.AiApprove` | AI Live Studio: Approve draft | AI Live Studio: 下書きを承認 | AI Live Studio: Setujui draf |

## 9. UX acceptance for the plugin

- A first-time user goes from "no Parallaxa" to a gently moving scene with Tools > Parallaxa: Set up current scene, or with three clicks (Filters > + > Parallaxa Camera Rig), without reading docs.
- Every property in sections 1 to 3 has its label and `.Desc` tooltip in all three languages; no raw key ever shows in the OBS UI.
- No property shows shader units: amounts are 0 to 100 %, depth is −100 to +100 with 0 = character, times are ms.
- Removing the rig, switching scene collection, exiting OBS and Tools > Restore all rest poses leave every item exactly at its rest pose (within 0.5 px).
- With Live-safe on, no input (panel, hotkey, vendor request, Look) exceeds 20°/s rotation, 0.5x/s zoom, ±8° roll, or shows a frame edge, and no more than 3 flashes happen in any second.
- Units round-trip: `fx_set` with params `{"vignette": 0.3}` shows Vignette amount 30 % in the panel, the shader's vignette uniform is 0.3, and Save as a user preset writes `"vignette": 0.3` (no rounding). Applying every shipped FX preset and saving it as a user preset gives the file's values back (absent params at their neutral value).
- Depth blur driven state: with Cozy Chat applied (depth of field 20 %) and no input for 60 s, Parallaxa Depth Blur on the background still shows `Stage.Info.DepthBlurDriven` with Strength greyed, and moving Strength through obs-websocket changes nothing on screen. Unticking Depth of field enables Strength and the layer renders the manual strength on the next frame. Deleting the rig while OBS runs, or reopening a scene collection whose rig was removed, leaves the layer not driven.
- Depth of field: with default settings, the Rack focus to background hotkey visibly moves the focus (Focus follows character unticks), and a dock `dof_set` with `amount` > 0 ticks the Depth of field group.
