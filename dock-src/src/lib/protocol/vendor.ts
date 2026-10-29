// Copyright (C) 2026 Parallaxa Labs
// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * Parallaxa obs-websocket vendor protocol v1: TypeScript types and constants.
 *
 * Hand-written mirror of docs/protocol/vendor-api.md (normative) and its JSON
 * Schemas (docs/protocol/schemas: vendor.*.json, move.json, recipe.json,
 * fx-preset.json); include/stage-protocol.h is the plugin's C mirror. No
 * runtime dependencies: only constants, types and small pure helpers.
 */

export const VENDOR_NAME = 'parallaxa';
export const VENDOR_PROTOCOL = 1;

export const FILTER_CAMERA_RIG = 'stage_camera_rig';
export const FILTER_FX = 'stage_fx';
export const FILTER_DEPTH_BLUR = 'stage_depth_blur';
export const DEPTH_BLUR_KEY_AUTO_STRENGTH = 'auto_strength';

export const VENDOR_REQUESTS = [
  'get_info',
  'get_state',
  'camera_move',
  'camera_set',
  'camera_reset',
  'camera_auto',
  'fx_set',
  'fx_pulse',
  'dof_set',
  'layer_list',
  'layer_set',
  'move_list',
  'recipe_list',
  'recipe_apply',
  'view_save',
  'view_apply',
  'view_list',
  'view_delete',
  'freeze',
  'panic',
] as const;
export type VendorRequest = (typeof VENDOR_REQUESTS)[number];

export const VENDOR_EVENTS = ['state_changed', 'move_finished', 'rig_changed', 'perf_warning', 'hotkey_forward'] as const;
export type VendorEvent = (typeof VENDOR_EVENTS)[number];

export const VENDOR_ERRORS = ['bad_param', 'no_rig', 'no_fx', 'not_found', 'busy', 'internal'] as const;
export type VendorError = (typeof VENDOR_ERRORS)[number];

export const FEATURES = ['camera', 'fx', 'audio', 'dof', 'views'] as const;
export type Feature = (typeof FEATURES)[number];

export const MOVES = ['zoom_in', 'zoom_out', 'pan_left', 'pan_right', 'shake', 'punch', 'dolly', 'nod', 'reset'] as const;
export type BuiltinMove = (typeof MOVES)[number];

export const EASES = [
  'linear',
  'ease_in_quad',
  'ease_out_quad',
  'ease_in_out_quad',
  'ease_in_cubic',
  'ease_out_cubic',
  'ease_in_out_cubic',
  'ease_in_quint',
  'ease_out_quint',
  'ease_in_out_quint',
  'ease_in_back',
  'ease_out_back',
  'ease_in_out_back',
  'ease_in_elastic',
  'ease_out_elastic',
  'ease_in_out_elastic',
  'smoothstep',
] as const;
export type Ease = (typeof EASES)[number];
export const EASE_DEFAULT: Ease = 'ease_in_out_cubic';

export const AUTO_MODES = ['off', 'drift', 'chatting', 'singing'] as const;
export type AutoMode = (typeof AUTO_MODES)[number];

export const ROLES = ['none', 'bg', 'char', 'fg', 'sky', 'static'] as const;
export type Role = (typeof ROLES)[number];

export const TRANSITIONS = ['cut', 'smooth', 'whip', 'punch'] as const;
export type Transition = (typeof TRANSITIONS)[number];

export const PULSE_EFFECTS = ['flash', 'chroma_kick', 'vignette_pulse'] as const;
export type PulseEffect = (typeof PULSE_EFFECTS)[number];

export const HOTKEY_FORWARD_ACTIONS = ['ai_stop', 'ai_skip', 'ai_mute', 'ai_panic', 'ai_approve'] as const;
export type HotkeyForwardAction = (typeof HOTKEY_FORWARD_ACTIONS)[number];

export const PERF_PAUSED = ['', 'bloom', 'lut', 'leak', 'grain', 'chroma'] as const;
export type PerfPaused = (typeof PERF_PAUSED)[number];

// Limits (mirror stage-protocol.h).
export const DEPTH_MIN = 0.1;
export const DEPTH_MAX = 10;
export const DEPTH_CHAR = 1;
export const UI_DEPTH_MIN = -100;
export const UI_DEPTH_MAX = 100;
/** d = 10^(-v/UI_DEPTH_SCALE): ±UI_DEPTH_MAX lands exactly on DEPTH_MIN/DEPTH_MAX. */
export const UI_DEPTH_SCALE = 100;
export const ZOOM_MIN = 0.5;
export const ZOOM_MAX = 4;
export const ROT_MIN = -45;
export const ROT_MAX = 45;
export const MOVE_DURATION_MIN = 50;
export const MOVE_DURATION_MAX = 10000;
export const SET_DURATION_MAX = 10000;
export const PULSE_DURATION_MIN = 50;
export const PULSE_DURATION_MAX = 2000;
export const VIEW_DURATION_DEFAULT = 600;
export const MOVE_INTENSITY_MAX = 2;
export const VIEW_SLOT_MIN = 1;
export const VIEW_SLOT_MAX = 9;
/** Unicode code points (count with `[...s].length`, not `s.length`); see viewNameValid. */
export const VIEW_NAME_MAX = 47;
/** Solver per-layer zoom factor clamp (vendor-api.md section 3). */
export const LAYER_SCALE_MIN = 0.25;
export const LAYER_SCALE_MAX = 8;
export const ID_MAX_LEN = 31;
export const MOVE_MAX_KEYS = 16;
export const STATE_EVENT_MAX_HZ = 10;
export const FLASH_MAX_PER_SEC = 3;
export const PERF_BUDGET_MS = 2;
export const PERF_WINDOW_S = 5;
export const ID_PATTERN = /^[a-z0-9][a-z0-9_-]{0,30}$/;

/** UI depth slider v in [-100, 100] (−100 far, 0 character, +100 near) → depth. */
export function depthFromUi(v: number): number {
  if (Number.isNaN(v)) return DEPTH_CHAR;
  const c = Math.min(UI_DEPTH_MAX, Math.max(UI_DEPTH_MIN, v));
  const d = Math.pow(10, -c / UI_DEPTH_SCALE);
  return Math.min(DEPTH_MAX, Math.max(DEPTH_MIN, d));
}

/** Inverse of depthFromUi; result in [−100, 100]. */
export function uiFromDepth(d: number): number {
  const x = Number.isNaN(d) || d <= 0 ? DEPTH_MIN : Math.min(DEPTH_MAX, Math.max(DEPTH_MIN, d));
  const v = -UI_DEPTH_SCALE * Math.log10(x);
  return v === 0 ? 0 : v;
}

/** Solver per-layer zoom factor: clamp(zoom^f, LAYER_SCALE_MIN, LAYER_SCALE_MAX), f = 1/depth. */
export function layerScale(zoom: number, f: number): number {
  const z = Math.min(ZOOM_MAX, Math.max(ZOOM_MIN, zoom));
  const s = Math.pow(z, f);
  if (Number.isNaN(s)) return 1;
  return Math.min(LAYER_SCALE_MAX, Math.max(LAYER_SCALE_MIN, s));
}

/** Saved-view name check: at most VIEW_NAME_MAX code points, no NUL, no lone surrogates. */
export function viewNameValid(name: string): boolean {
  let n = 0;
  for (const ch of name) {
    const c = ch.charCodeAt(0);
    if (c === 0 || (ch.length === 1 && c >= 0xd800 && c <= 0xdfff)) return false;
    if (++n > VIEW_NAME_MAX) return false;
  }
  return true;
}

export interface LocalizedName {
  en: string;
  ja: string;
  id: string;
}

export interface VendorOk {
  ok: true;
}
export interface VendorFailure {
  ok: false;
  code: VendorError;
  error: string;
}
export type VendorResponse<T extends object = object> = (VendorOk & T) | VendorFailure;
export interface CmdResult {
  id: number;
}

export interface CameraPose {
  x: number;
  y: number;
  zoom: number;
  rot: number;
}
export interface CameraState extends CameraPose {
  shake: number;
}
export interface AutoState {
  mode: AutoMode;
  intensity: number;
}
export interface FxState {
  preset: string;
}
export interface DofState {
  focus_depth: number;
  amount: number;
}
export interface FxParams {
  exposure?: number;
  contrast?: number;
  saturation?: number;
  temperature?: number;
  tint?: number;
  vignette?: number;
  vignette_soft?: number;
  vignette_round?: number;
  chroma?: number;
  grain?: number;
  grain_size?: number;
  lut?: string;
  lut_amount?: number;
  leak?: string;
  leak_amount?: number;
  leak_speed?: number;
  bloom?: number;
  bloom_threshold?: number;
}

/** Rig state: get_state (plus ok) and the state_changed / stage.state event. */
export interface StageState {
  scene: string;
  scene_uuid: string;
  camera: CameraState;
  auto: AutoState;
  fx: FxState;
  dof: DofState;
  moving: boolean;
  move?: string;
  frozen: boolean;
  view: number;
}
export interface RigInfo {
  scene: string;
  scene_uuid: string;
  enabled: boolean;
}
export interface FxInfo {
  source: string;
  source_uuid: string;
  enabled: boolean;
}
export interface StageLayer {
  item_id: number;
  name: string;
  source_uuid: string;
  /**
   * Present only while the item is enrolled (role is not 'none'). obs_data has no
   * null (vendor-api.md 1.1), so the plugin omits it; treat null like absent.
   */
  depth?: number | null;
  role: Role;
  visible: boolean;
  locked: boolean;
  depth_blur: boolean;
}
export interface MoveInfo {
  id: string;
  name: LocalizedName;
  duration_ms: number;
  builtin: boolean;
}
export interface RecipeInfo {
  id: string;
  name: LocalizedName;
  pro: boolean;
  /** Optional; never sent by Parallaxa over obs-websocket (obs_data has no string arrays, vendor-api.md 1.1). */
  tags?: string[];
  user: boolean;
}
export interface ViewInfo {
  slot: number;
  name: string;
  camera: CameraPose;
  transition: Transition;
  duration_ms: number;
}

/** requestData and successful responseData (without ok) per vendor request. */
export interface VendorRequests {
  get_info: {
    req: Record<string, never>;
    res: {
      version: string;
      protocol: number;
      obs: string;
      canvas: { width: number; height: number };
      /** Optional; never sent by Parallaxa over obs-websocket (vendor-api.md 1.1). Absent = not reported. */
      features?: Feature[];
      rigs: RigInfo[];
      fx: FxInfo[];
    };
  };
  get_state: { req: { scene?: string }; res: StageState };
  camera_move: {
    req: { scene?: string; move: BuiltinMove | string; duration_ms?: number; intensity?: number; ease?: Ease };
    res: CmdResult;
  };
  camera_set: {
    req: {
      scene?: string;
      x?: number;
      y?: number;
      zoom?: number;
      rot?: number;
      relative?: boolean;
      duration_ms?: number;
      ease?: Ease;
    };
    res: CmdResult;
  };
  camera_reset: { req: { scene?: string; duration_ms?: number }; res: CmdResult };
  camera_auto: { req: { scene?: string; mode: AutoMode; intensity?: number }; res: CmdResult };
  fx_set: { req: { scene?: string; preset?: string; params?: FxParams; duration_ms?: number }; res: CmdResult };
  fx_pulse: { req: { scene?: string; effect: PulseEffect; intensity?: number; duration_ms?: number }; res: CmdResult };
  dof_set: {
    req: { scene?: string; focus_depth?: number; amount?: number; duration_ms?: number; ease?: Ease };
    res: CmdResult;
  };
  layer_list: { req: { scene?: string }; res: { scene: string; scene_uuid: string; layers: StageLayer[] } };
  layer_set: { req: { scene?: string; item_id: number; depth?: number | null; role?: Role }; res: CmdResult };
  move_list: { req: Record<string, never>; res: { moves: MoveInfo[] } };
  recipe_list: { req: Record<string, never>; res: { recipes: RecipeInfo[] } };
  recipe_apply: {
    req: { scene?: string; recipe: string; duration_ms?: number };
    res: CmdResult & { applied: { layers: number; auto: AutoMode; fx: string } };
  };
  view_save: {
    req: { scene?: string; slot: number; name?: string; transition?: Transition; duration_ms?: number; keep_pose?: boolean };
    res: { view: ViewInfo };
  };
  view_apply: { req: { scene?: string; slot: number; transition?: Transition; duration_ms?: number }; res: CmdResult };
  view_list: { req: { scene?: string }; res: { scene: string; scene_uuid: string; views: ViewInfo[] } };
  view_delete: { req: { scene?: string; slot: number }; res: Record<string, never> };
  freeze: { req: { scene?: string; on?: boolean }; res: { frozen: boolean } };
  panic: { req: Record<string, never>; res: Record<string, never> };
}

export interface MoveFinishedEvent {
  scene: string;
  scene_uuid: string;
  id: number;
  move: string;
  cancelled: boolean;
}
export interface PerfWarningEvent {
  source: string;
  source_uuid: string;
  filter: typeof FILTER_FX | typeof FILTER_DEPTH_BLUR;
  cost_ms: number;
  budget_ms: number;
  window_s: number;
  paused: PerfPaused;
}
export interface HotkeyForwardEvent {
  action: HotkeyForwardAction;
}

/** Preset files (data/presets). */
export interface MoveKey {
  t: number;
  dx?: number;
  dy?: number;
  dzoom?: number;
  drot?: number;
  shake?: number;
  ease?: Ease;
}
export interface MoveFile {
  id: string;
  name: LocalizedName;
  duration_ms: number;
  relative?: boolean;
  hold?: boolean;
  pro?: boolean;
  tags?: string[];
  keys: MoveKey[];
}
export interface RecipeFile {
  id: string;
  name: LocalizedName;
  description?: LocalizedName;
  pro: boolean;
  tags?: string[];
  roles: Partial<Record<'bg' | 'char' | 'fg', number>>;
  auto?: AutoState;
  fx?: string;
  audio_react?: { amount: number; attack_ms?: number; release_ms?: number };
  dof?: DofState;
  camera?: CameraPose;
  overscan?: number;
}
export interface FxPresetFile {
  id: string;
  name: LocalizedName;
  pro?: boolean;
  tags?: string[];
  params: FxParams;
}
