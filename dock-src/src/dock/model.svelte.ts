// Copyright (C) 2026 Parallaxa Labs
// SPDX-License-Identifier: GPL-2.0-or-later
// Stage control state and actions, shared by AI Live Studio's Stage dock and
// the standalone dock. Everything goes through a StageHost (host.ts);
// this file builds the documented vendor payloads (vendor-api.md 4, 5).
//
// Live-safe rules kept here (not only in the UI):
// - On-air lock: while OBS streams, picture-moving sliders (camera, depth, DoF,
//   effects, the LUT, intensity) and role edits are refused unless Alt is held or
//   "Unlock for 10 s" runs. Pads, Home, Freeze, moves, Looks, pulses,
//   All effects off and Panic always work.
// - Undo: the last 20 slider changes, replayed with their previous values
//   (only values this page knew), on the scene they were made in. Scene item ids
//   and poses belong to one scene, so the stack is dropped when the target or
//   program scene changes, and a replay names its scene explicitly.
// - Photosensitivity: at most 3 pulses per rolling second leave the page
//   (WCAG 2.3.1; the plugin enforces the same cap on its side).
import {
  depthFromUi,
  uiFromDepth,
  VIEW_SLOT_MAX,
  FLASH_MAX_PER_SEC,
  type AutoMode,
  type CameraPose,
  type FxParams,
  type MoveInfo,
  type PulseEffect,
  type RecipeInfo,
  type Role,
  type StageLayer,
  type StageState,
  type Transition,
  type VendorRequest,
  type VendorRequests,
  type ViewInfo,
} from '../lib/protocol/vendor.ts';
import { HostError, type HostEvents, type Req, type StageHost } from './host.ts';
import { t } from './t.ts';

export const UNDO_MAX = 20;
export const UNLOCK_S = 10;
export const EASE_RELEASE = 'ease_out_cubic';
/** Default rack focus target when no background layer has a depth (vendor-api.md 3: bg default 3). */
const RACK_BG_DEPTH = 3;

export type Info = VendorRequests['get_info']['res'];
type Ok<R extends VendorRequest> = VendorRequests[R]['res'];
export interface Notice {
  text: string;
  error?: boolean;
}
interface UndoEntry {
  label: string;
  run: () => unknown;
  /** Scene name or uuid the change was made in ('' = unknown). */
  scene: string;
}

/** Role guess from an item name, as the plugin's recipe_apply does (vendor-api.md 4.13). */
export function guessRole(name: string): Role {
  const word = (w: string) => new RegExp(`(^|[^a-z])${w}([^a-z]|$)`, 'i').test(name);
  if (/background|背景/i.test(name) || word('bg')) return 'bg';
  if (/foreground|前景/i.test(name) || word('fg')) return 'fg';
  if (/vtube|spout|avatar|model/i.test(name) || word('vts')) return 'char';
  return 'none';
}

export class StageModel {
  readonly host: StageHost;
  info = $state.raw<Info | null>(null);
  st = $state.raw<StageState | null>(null);
  layers = $state.raw<StageLayer[]>([]);
  moves = $state.raw<MoveInfo[]>([]);
  recipes = $state.raw<RecipeInfo[]>([]);
  /** Saved views by slot - 1 (undefined = empty slot). */
  views = $state.raw<(ViewInfo | undefined)[]>([]);
  noRig = $state.raw(false);
  /** The last FX request answered no_fx (cleared on every scene load). */
  fxRefused = $state.raw(false);
  notice = $state.raw<Notice | null>(null);
  /** Alt is held (temporary unlock). */
  alt = $state.raw(false);
  /** Seconds left of "Unlock for 10 s". */
  unlockLeft = $state.raw(0);
  undos = $state.raw<UndoEntry[]>([]);
  /** FX values sent from this page since the last preset change (what a slider shows first). */
  fxSent = $state.raw<FxParams>({});
  /** The built-in preset the slider values derive from (rule 2); '' = unknown. */
  fxBase = $state.raw('');
  /** Restore value for the depth of field toggle. */
  dofRestore = 0.3;

  #pulses: number[] = [];
  #unlockTimer: ReturnType<typeof setInterval> | undefined;
  #noticeTimer: ReturnType<typeof setTimeout> | undefined;
  #gen = 0;
  /** Scene a running undo replay targets (sc() returns it). */
  #pin: string | undefined;
  /** Target + program scene the undo stack belongs to. */
  #undoAt = '';

  constructor(host: StageHost) {
    this.host = host;
  }

  /** On-air lock: OBS streams, Alt is not held and "Unlock for 10 s" is not running. */
  get locked(): boolean {
    return this.host.streaming && !this.alt && this.unlockLeft <= 0;
  }

  /** `scene` field for requests ('' = program scene: omitted). */
  sc(): { scene?: string } {
    const s = this.#pin ?? this.host.scene;
    return s ? { scene: s } : {};
  }

  /**
   * Where fx_set / fx_pulse go (vendor-api.md 2): the target scene when it carries
   * stage_fx, else a source of this scene that does. null = no Stage FX here.
   */
  fxTarget(): { scene?: string } | null {
    const fx = this.info?.fx.filter((f) => f.enabled);
    const s = this.host.scene;
    const uuid = this.st?.scene_uuid;
    if (fx && !fx.length) return null;
    if (!fx || (!s && !uuid)) return this.sc(); // not known yet: the plugin decides
    if (fx.some((f) => f.source_uuid === uuid || f.source === s || f.source_uuid === s)) return this.sc();
    const src = fx.find((f) => this.layers.some((l) => l.source_uuid === f.source_uuid));
    return src ? { scene: src.source_uuid } : null;
  }

  /** The Effects card is off: no Stage FX for this scene, or the plugin just said so. */
  get noFx(): boolean {
    return this.fxRefused || !this.fxTarget();
  }

  // ---- transport ----

  /**
   * Calls a vendor request; on failure shows a notice and resolves null. The notice names
   * the request and the error code when there is no friendlier sentence for it.
   * `quiet` (loads the page starts by itself, not a user action): no "couldn't do that";
   * states the UI already shows (no rig, no FX, Stage missing or older, offline) say
   * nothing, any other failure says what could not be read.
   * `codes` gives this request its own sentence for an error code.
   */
  async call<R extends VendorRequest>(req: R, data?: Req<R>, notFound = 'stage.failed', quiet = false, codes: Record<string, string> = {}): Promise<Ok<R> | null> {
    let code: string;
    let key: string | undefined;
    try {
      const r = await this.host.call(req, data);
      if (r.ok) return r as unknown as Ok<R>;
      code = r.code;
      if (code === 'no_rig') this.noRig = true;
      if (code === 'no_fx') this.fxRefused = true;
      key = codes[code] ?? { no_rig: 'stage.no_rig', no_fx: 'stage.no_fx', busy: 'stage.busy', not_found: notFound === 'stage.failed' ? undefined : notFound }[code];
    } catch (e) {
      code = e instanceof HostError ? e.code : 'internal';
      key = { no_stage: 'stage.not_found', disconnected: 'stage.offline', unavailable: 'stage.offline', unsupported: 'stage.update_needed' }[code];
    }
    if (!quiet) this.say(key ? t(key) : t('stage.failed_code', { request: req, code }), true);
    else if (!key) this.say(t('stage.load_failed', { request: req, code }), true);
    return null;
  }

  /** Shows a short notice (errors stay a little longer). */
  say(text: string, error = false): void {
    this.notice = { text, error };
    clearTimeout(this.#noticeTimer);
    this.#noticeTimer = setTimeout(() => (this.notice = null), error ? 8000 : 5000);
  }

  // ---- loading and events ----

  /** Loads everything (on connect and on "Reload"). */
  async load(): Promise<void> {
    const q = 'stage.failed';
    const [info, moves, recipes] = await Promise.all([this.call('get_info', {}, q, true), this.call('move_list', {}, q, true), this.call('recipe_list', {}, q, true)]);
    this.info = info;
    // Built-in moves first, each group in the plugin's order.
    if (moves) this.moves = [...moves.moves].sort((a, b) => Number(b.builtin) - Number(a.builtin));
    if (recipes) this.recipes = recipes.recipes;
    await this.loadScene();
  }

  /** Reloads what belongs to the target scene: state, layers, views. */
  async loadScene(): Promise<void> {
    const gen = ++this.#gen;
    const sc = this.sc();
    this.fxSent = {};
    this.fxRefused = false;
    this.#undoScene();
    try {
      const r = await this.host.call('get_state', sc);
      if (gen !== this.#gen) return;
      this.noRig = !r.ok && r.code === 'no_rig';
      if (r.ok) this.setState(r);
      else {
        this.st = null;
        // Not "no rig" (shown as its own warning) nor a missing target scene: say what failed.
        if (!this.noRig && r.code !== 'not_found') this.say(t('stage.load_failed', { request: 'get_state', code: r.code }), true);
      }
    } catch {
      return; // offline or Stage missing: the connection banner explains
    }
    if (this.noRig) {
      this.layers = [];
      this.views = [];
      return;
    }
    const [layers, views] = await Promise.all([this.call('layer_list', sc, 'stage.failed', true), this.call('view_list', sc, 'stage.failed', true)]);
    if (gen !== this.#gen) return;
    this.layers = layers?.layers ?? [];
    const v: (ViewInfo | undefined)[] = [];
    for (const x of views?.views ?? []) v[x.slot - 1] = x;
    this.views = v;
  }

  setState(d: StageState): void {
    const prev = this.st;
    this.st = d;
    this.#undoScene();
    if (!prev || prev.fx.preset !== d.fx.preset || prev.scene_uuid !== d.scene_uuid) {
      // A new preset (or Look, or All effects off) resets the values sent here.
      // An empty preset right after our own edits keeps the base: the plugin clears
      // the preset name when params change.
      if (d.fx.preset || prev?.scene_uuid !== d.scene_uuid || !Object.keys(this.fxSent).length) {
        this.fxBase = d.fx.preset;
        this.fxSent = {};
      }
    }
    if (d.dof.amount > 0) this.dofRestore = d.dof.amount;
  }

  /** Subscribes to host events; returns the cleanup. */
  listen(): () => void {
    const h = this.host;
    const offs = [
      h.on('state_changed', (d) => this.onState(d)),
      h.on('rig_changed', (d) => this.onRig(d)),
      h.on('perf_warning', (d) => d.paused && this.say(t('stage.perf_paused', { effect: t(`stage.fx.${d.paused}`) }))),
      h.on('program_scene', () => {
        if (h.scene) return;
        this.undos = []; // the program scene moved: old entries name another scene's items
        void this.loadScene();
      }),
    ];
    return () => {
      offs.forEach((f) => f());
      clearInterval(this.#unlockTimer);
      clearTimeout(this.#noticeTimer);
    };
  }

  #mine(scene: string, uuid: string): boolean {
    const s = this.host.scene;
    if (s) return scene === s || uuid === s;
    return !this.st || uuid === this.st.scene_uuid;
  }

  /** Another rig's state is ignored: a program-scene switch arrives as `program_scene`. */
  onState(d: HostEvents['state_changed']): void {
    if (this.#mine(d.scene, d.scene_uuid)) this.setState(d);
  }

  onRig(d: HostEvents['rig_changed']): void {
    void this.call('get_info', {}, 'stage.failed', true).then((i) => i && (this.info = i));
    if (this.#mine(d.scene, d.scene_uuid) || this.noRig) void this.loadScene();
  }

  // ---- on-air lock ----

  /** "Unlock for 10 s". */
  unlock(): void {
    clearInterval(this.#unlockTimer);
    this.unlockLeft = UNLOCK_S;
    this.#unlockTimer = setInterval(() => {
      if (--this.unlockLeft <= 0) clearInterval(this.#unlockTimer);
    }, 1000);
  }

  /** Refuses a locked change with a notice; true when the change may go. */
  free(): boolean {
    if (!this.locked) return true;
    this.say(t('stage.locked_hint'));
    return false;
  }

  // ---- undo ----

  /** Drops the undo stack when the target or program scene changed; false when it did. */
  #undoScene(): boolean {
    const at = `${this.host.scene}
${this.st?.scene_uuid ?? ''}`;
    if (at === this.#undoAt) return true;
    this.#undoAt = at;
    if (this.undos.length) this.undos = [];
    return false;
  }

  /** Remembers how to undo a slider change (sliders call it on commit, with the value before). */
  remember(label: string, run: () => unknown): void {
    this.#undoScene();
    const scene = this.host.scene || this.st?.scene_uuid || '';
    this.undos = [...this.undos.slice(1 - UNDO_MAX), { label, run, scene }];
  }

  async undo(): Promise<void> {
    const last = this.undos.at(-1);
    if (!last || !this.#undoScene() || !this.free()) return;
    this.undos = this.undos.slice(0, -1);
    // The model methods read sc() before their first await, so the pin covers the request.
    this.#pin = last.scene || undefined;
    let p: unknown;
    try {
      p = last.run();
    } finally {
      this.#pin = undefined;
    }
    await p;
    this.say(t('stage.undone', { what: last.label }));
  }

  // ---- camera ----

  /** Absolute camera pose; `duration` 0 follows through the rig's smoothing only. */
  camera(p: Partial<CameraPose>, duration = 0, ease?: string): Promise<unknown> {
    const data: Req<'camera_set'> = { ...this.sc(), ...p, duration_ms: duration };
    if (ease) data.ease = ease as Req<'camera_set'>['ease'];
    return this.call('camera_set', data);
  }

  /** Arrow-key nudge (relative camera_set). */
  nudge(p: Partial<CameraPose>): void {
    if (this.free()) void this.call('camera_set', { ...this.sc(), ...p, relative: true });
  }

  home(): void {
    void this.call('camera_reset', this.sc());
  }

  async freeze(): Promise<void> {
    const r = await this.call('freeze', { ...this.sc(), on: !this.st?.frozen });
    if (r && this.st) this.st = { ...this.st, frozen: r.frozen };
  }

  auto(mode: AutoMode, intensity?: number): void {
    const data: Req<'camera_auto'> = { ...this.sc(), mode };
    if (intensity !== undefined) data.intensity = intensity;
    void this.call('camera_auto', data);
  }

  move(id: string): void {
    void this.call('camera_move', { ...this.sc(), move: id });
  }

  // ---- views ----

  applyView(slot: number): void {
    if (!this.views[slot - 1]) return;
    void this.call('view_apply', { ...this.sc(), slot }, 'stage.view_empty').then((r) => {
      if (!r) void this.loadScene(); // the slot may have been deleted elsewhere
    });
  }

  /**
   * keepPose edits the stored view (name, transition) without taking the current camera.
   * An edit leaves an omitted name as it is, so a cleared name sends the plugin's default;
   * a plugin older than keep_pose answers bad_param, which means "update" (never a plain save).
   */
  async saveView(slot: number, name: string, transition: Transition, duration: number, keepPose = false): Promise<boolean> {
    const data: Req<'view_save'> = { ...this.sc(), slot, transition, duration_ms: duration };
    if (name.trim() || keepPose) data.name = name.trim() || `View ${slot}`;
    if (keepPose) data.keep_pose = true;
    const r = await (keepPose
      ? this.call('view_save', data, 'stage.view_empty', false, { bad_param: 'stage.update_needed' })
      : this.call('view_save', data));
    if (r) this.#setView(slot, r.view);
    return !!r;
  }

  async deleteView(slot: number): Promise<void> {
    if (await this.call('view_delete', { ...this.sc(), slot })) this.#setView(slot, undefined);
  }

  #setView(slot: number, v: ViewInfo | undefined): void {
    const next = this.views.slice();
    next[slot - 1] = v;
    this.views = next;
  }

  // ---- layers ----

  async role(item: StageLayer, role: Role): Promise<void> {
    if (!this.free()) return;
    await this.call('layer_set', { ...this.sc(), item_id: item.item_id, role });
    await this.#reloadLayers(); // also puts the select back when Stage refused the role
  }

  /** Sends a depth from the UI slider value v (-100..100). */
  depth(item: StageLayer, v: number): void {
    if (!this.free()) return;
    const depth = depthFromUi(v);
    this.layers = this.layers.map((l) => (l.item_id === item.item_id ? { ...l, depth } : l));
    void this.call('layer_set', { ...this.sc(), item_id: item.item_id, depth });
  }

  /** Auto-assign roles: name guesses for items that have no role yet. Returns how many got one. */
  async autoAssign(): Promise<number> {
    if (!this.free()) return 0;
    let n = 0;
    for (const l of this.layers) {
      const role = guessRole(l.name);
      if (l.role !== 'none' || role === 'none') continue;
      if (await this.call('layer_set', { ...this.sc(), item_id: l.item_id, role })) n++;
    }
    this.say(t('stage.assigned', { count: n }));
    if (n) await this.#reloadLayers();
    return n;
  }

  async #reloadLayers(): Promise<void> {
    const r = await this.call('layer_list', this.sc());
    if (r) this.layers = r.layers;
  }

  // ---- depth of field (stage-plugin-ux.md 1 "Depth of field state") ----

  dof(p: { focus?: number; amount?: number }, duration = 0): Promise<unknown> {
    const data: Req<'dof_set'> = { ...this.sc() };
    if (p.focus !== undefined) data.focus_depth = depthFromUi(p.focus);
    if (p.amount !== undefined) data.amount = p.amount;
    if (duration) {
      data.duration_ms = duration;
      data.ease = 'ease_in_out_quint';
    }
    if (this.st) this.st = { ...this.st, dof: { focus_depth: data.focus_depth ?? this.st.dof.focus_depth, amount: data.amount ?? this.st.dof.amount } };
    return this.call('dof_set', data);
  }

  /** Rack focus: character plane <-> the farthest blurred background (animated). */
  rack(): void {
    const cur = this.st?.dof.focus_depth ?? 1;
    // Only layers with a Stage Depth Blur filter can go out of focus; behind the character = depth > 1.
    const far = Math.max(0, ...this.layers.filter((l) => l.depth_blur && (l.depth ?? 0) > 1).map((l) => l.depth as number));
    const target = Math.abs(cur - 1) > 0.01 ? 1 : far || RACK_BG_DEPTH;
    void this.dof({ focus: uiFromDepth(target) }, 1200);
  }

  // ---- effects ----

  fxPreset(preset: string): void {
    void this.call('fx_set', { ...this.fxTarget(), preset });
  }

  /** One effect param (protocol units). The LUT select is locked too: a LUT switch is as visible as its amount slider. */
  fx(p: keyof FxParams, v: number | string): void {
    if (!this.free()) return;
    this.fxSent = { ...this.fxSent, [p]: v };
    void this.call('fx_set', { ...this.fxTarget(), params: { [p]: v } });
  }

  /** Pulse buttons, capped at FLASH_MAX_PER_SEC per rolling second. */
  pulse(effect: PulseEffect): boolean {
    const now = Date.now();
    this.#pulses = this.#pulses.filter((x) => now - x < 1000);
    if (this.#pulses.length >= FLASH_MAX_PER_SEC) {
      this.say(t('stage.flash_cap'));
      return false;
    }
    this.#pulses.push(now);
    void this.call('fx_pulse', { ...this.fxTarget(), effect });
    return true;
  }

  // ---- looks and panic ----

  async recipe(id: string): Promise<void> {
    const r = await this.call('recipe_apply', { ...this.sc(), recipe: id });
    if (r) await this.#reloadLayers();
  }

  /** Stage panic (vendor `panic`): camera home, auto off, freeze cleared, FX neutral, DoF off. */
  panic(): void {
    void this.call('panic', {});
  }
}

/** Slots 1..9. */
export const SLOTS = Array.from({ length: VIEW_SLOT_MAX }, (_, i) => i + 1);
