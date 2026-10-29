// Copyright (C) 2026 Parallaxa Labs
// SPDX-License-Identifier: GPL-2.0-or-later
// StageHost for the standalone dock (Parallaxa data/dock/dock-standalone.html,
// opened as a file URL in an OBS Custom Browser Dock): speaks obs-websocket v5
// directly (vendor-api.md 1). The address and password are entered once and
// kept in this OBS browser profile (localStorage). Auth uses the bundled
// SHA-256 (sha256.ts): a file page is not a secure context.
//
// Known limit (accepted): the password is stored in plain
// text, and file:// pages share one localStorage origin in the OBS browser
// profile, so any other local file page opened in an OBS dock or browser
// source can read it. "Forget password" removes it. The obs-websocket password
// only guards a loopback port, and the dock cannot keep a secret from a page
// running in the same profile anyway.
import { VENDOR_NAME, type VendorRequest } from '../lib/protocol/vendor.ts';
import { Emitter, HostError, type Conn, type HostEvents, type Lang, type Req, type Res, type StageHost } from './host.ts';
import { obsAuth } from './sha256.ts';

export const DEFAULT_URL = 'ws://127.0.0.1:4455';
export const SETTINGS_KEY = 'parallaxa.dock';
/** Event subscriptions (obs-websocket EventSubscription): Scenes | Outputs | Vendors. */
export const SUBSCRIPTIONS = (1 << 2) | (1 << 6) | (1 << 9);
/** obs-websocket close code for a wrong password. */
const AUTH_FAILED = 4009;
/**
 * obs-websocket request status ResourceNotFound. For CallVendorRequest it means either
 * "No vendor was found by that name." (Stage not loaded) or that the vendor has no such
 * request (an older Stage): only the comment tells them apart.
 */
const NOT_FOUND = 600;
/** Delay before asking Stage again after a failed get_info (timeout, error answer). */
export const PROBE_RETRY_MS = 3000;

export interface DockSettings {
  url: string;
  password: string;
  lang: 'auto' | Lang;
  motion: boolean;
  scene: string;
}

/** The subset of WebSocket used here (a fake in tests). */
export interface Sock {
  readyState: number;
  onopen: ((e: unknown) => void) | null;
  onmessage: ((e: { data: unknown }) => void) | null;
  onclose: ((e: { code: number }) => void) | null;
  onerror: ((e: unknown) => void) | null;
  send(data: string): void;
  close(code?: number): void;
}

type Store = Pick<Storage, 'getItem' | 'setItem'>;

function storage(): Store | null {
  try {
    return localStorage;
  } catch {
    return null;
  }
}

export function loadSettings(store: Store | null = storage()): DockSettings {
  // Reduce motion starts from the Windows setting; after that the toggle is explicit.
  const os = typeof matchMedia !== 'undefined' && matchMedia('(prefers-reduced-motion: reduce)').matches;
  const d: DockSettings = { url: DEFAULT_URL, password: '', lang: 'auto', motion: os, scene: '' };
  try {
    const s = JSON.parse(store?.getItem(SETTINGS_KEY) ?? '{}') as Partial<DockSettings>;
    for (const k of Object.keys(d) as (keyof DockSettings)[]) if (typeof s[k] === typeof d[k]) (d as unknown as Record<string, unknown>)[k] = s[k];
  } catch {
    /* corrupt or blocked storage: defaults */
  }
  return d;
}

/** Browser language, `in` being the legacy code for Indonesian. */
export function detectLang(prefs: readonly string[]): Lang {
  for (const p of prefs) {
    const b = p.toLowerCase().slice(0, 2);
    if (b === 'in' || b === 'id') return 'id';
    if (b === 'ja' || b === 'en') return b;
  }
  return 'en';
}

interface Pending {
  ok: (d: { requestStatus: { result: boolean; code: number; comment?: string }; responseData?: Record<string, unknown> }) => void;
  fail: (e: HostError) => void;
  timer: ReturnType<typeof setTimeout>;
}

export class ObsHost implements StageHost {
  conn = $state.raw<Conn>('connecting');
  streaming = $state.raw(false);
  epoch = $state.raw(0);
  /** The stored password was refused (shown in the connect form). */
  authFailed = $state.raw(false);
  settings = $state.raw<DockSettings>(loadSettings(null));

  #em = new Emitter();
  #ws: Sock | null = null;
  #seq = 0;
  #pending = new Map<string, Pending>();
  #retry: ReturnType<typeof setTimeout> | undefined;
  #attempt = 0;
  #identified = false;
  #make: (url: string) => Sock;
  #store: Store | null;

  constructor(opts: { socket?: (url: string) => Sock; store?: Store | null } = {}) {
    this.#make = opts.socket ?? ((u) => new WebSocket(u) as unknown as Sock);
    this.#store = opts.store === undefined ? storage() : opts.store;
    this.settings = loadSettings(this.#store);
    this.#applyMotion();
  }

  get lang(): Lang {
    const l = this.settings.lang;
    return l !== 'auto' ? l : detectLang(typeof navigator === 'undefined' ? [] : (navigator.languages ?? [navigator.language]));
  }
  get scene() {
    return this.settings.scene;
  }
  get reduceMotion() {
    return this.settings.motion;
  }
  setReduceMotion(on: boolean): void {
    this.save({ motion: on });
    this.#applyMotion();
  }
  setScene(scene: string): void {
    this.save({ scene });
  }

  /** Updates and stores settings (this OBS browser profile only). */
  save(patch: Partial<DockSettings>): void {
    this.settings = { ...this.settings, ...patch };
    try {
      this.#store?.setItem(SETTINGS_KEY, JSON.stringify(this.settings));
    } catch {
      /* blocked storage: works until the dock reloads */
    }
  }

  #applyMotion(): void {
    if (typeof document === 'undefined') return;
    // Explicit both ways: "Off" must also override the OS preference (tokens.css).
    document.documentElement.dataset.motion = this.settings.motion ? 'reduce' : 'full';
  }

  // ---- connection ----

  /** Connects (or reconnects at once) with the stored address and password. */
  connect(): void {
    clearTimeout(this.#retry);
    this.#drop();
    // While the server stays off, retries keep saying so (no flicker to "Looking for Stage").
    if (this.conn !== 'ws_off') this.conn = 'connecting';
    let ws: Sock;
    try {
      ws = this.#make(this.settings.url);
    } catch {
      this.conn = 'ws_off'; // malformed address: the form shows it
      return;
    }
    this.#ws = ws;
    this.#identified = false;
    ws.onmessage = (e) => {
      if (typeof e.data !== 'string') return;
      try {
        this.#handle(ws, JSON.parse(e.data) as { op: number; d: Record<string, unknown> });
      } catch (err) {
        console.error('[stage] bad obs-websocket frame', err);
      }
    };
    ws.onclose = (e) => {
      if (this.#ws !== ws) return;
      this.#ws = null;
      this.#failAll();
      // Unknown until GetStreamStatus after the next Identified (#probe sets it before
      // 'ready'): an OBS that crashed mid-stream is not on air.
      this.streaming = false;
      if (e.code === AUTH_FAILED) {
        this.authFailed = true;
        this.conn = 'auth';
        return;
      }
      if (this.conn === 'auth') return; // waiting for a password: no retry
      this.conn = this.#identified ? 'connecting' : 'ws_off';
      // OBS restarts, or its WebSocket server gets switched on: keep trying (1 s .. 10 s).
      this.#retry = setTimeout(() => this.connect(), Math.min(10_000, 1000 * 2 ** this.#attempt++));
    };
  }

  /** "Forget password": clears it and waits for a new one. */
  forget(): void {
    this.save({ password: '' });
    // A pending reconnect would leave 'auth' again and hide the password form.
    clearTimeout(this.#retry);
    this.#drop();
    this.authFailed = false;
    this.conn = 'auth';
  }

  /** Closes the socket for good (page teardown, tests). */
  close(): void {
    clearTimeout(this.#retry);
    this.#drop();
  }

  #drop(): void {
    const ws = this.#ws;
    this.#ws = null;
    if (ws) {
      ws.onmessage = ws.onclose = null;
      ws.close(1000);
    }
    this.#failAll();
  }

  #failAll(): void {
    for (const p of this.#pending.values()) {
      clearTimeout(p.timer);
      p.fail(new HostError('disconnected'));
    }
    this.#pending.clear();
  }

  #handle(ws: Sock, m: { op: number; d: Record<string, unknown> }): void {
    const d = m.d;
    switch (m.op) {
      case 0: {
        // Hello: identify, with auth when the server asks for it.
        const a = d.authentication as { challenge: string; salt: string } | undefined;
        if (a && !this.settings.password) {
          this.conn = 'auth';
          this.#drop();
          return;
        }
        const id: Record<string, unknown> = { rpcVersion: 1, eventSubscriptions: SUBSCRIPTIONS };
        if (a) id.authentication = obsAuth(this.settings.password, a.salt, a.challenge);
        ws.send(JSON.stringify({ op: 1, d: id }));
        return;
      }
      case 2:
        this.#identified = true;
        this.#attempt = 0;
        this.authFailed = false;
        void this.#probe();
        return;
      case 5: {
        const e = d.eventData as Record<string, unknown>;
        if (d.eventType === 'VendorEvent' && e.vendorName === VENDOR_NAME) {
          this.#em.emit(e.eventType as keyof HostEvents, e.eventData as never);
        } else if (d.eventType === 'StreamStateChanged') this.streaming = !!e.outputActive;
        else if (d.eventType === 'CurrentProgramSceneChanged') this.#em.emit('program_scene', { scene: String(e.sceneName) });
        return;
      }
      case 7: {
        const p = this.#pending.get(String(d.requestId));
        if (!p) return;
        this.#pending.delete(String(d.requestId));
        clearTimeout(p.timer);
        p.ok(d as never);
      }
    }
  }

  /**
   * After Identified: is Stage there, and is OBS streaming? A timeout or an error answer
   * asks again in PROBE_RETRY_MS (still "Looking for Stage"); no_stage waits for "Check
   * again", and a lost socket reconnects on its own.
   */
  async #probe(): Promise<void> {
    clearTimeout(this.#retry);
    const ws = this.#ws;
    try {
      const s = await this.#request('GetStreamStatus');
      this.streaming = !!s.responseData?.outputActive;
      const r = await this.call('get_info', {});
      if (r.ok) {
        this.conn = 'ready';
        this.epoch++;
        return;
      }
    } catch (e) {
      if (e instanceof HostError && (e.code === 'no_stage' || e.code === 'disconnected')) return;
    }
    if (this.#ws === ws && this.conn === 'connecting') this.#retry = setTimeout(() => void this.#probe(), PROBE_RETRY_MS);
  }

  #request(requestType: string, requestData?: object): Promise<Parameters<Pending['ok']>[0]> {
    const ws = this.#ws;
    if (!ws || !this.#identified) return Promise.reject(new HostError('disconnected'));
    const requestId = String(++this.#seq);
    return new Promise((ok, fail) => {
      const timer = setTimeout(() => {
        this.#pending.delete(requestId);
        fail(new HostError('timeout'));
      }, 5000);
      this.#pending.set(requestId, { ok, fail, timer });
      ws.send(JSON.stringify({ op: 6, d: { requestType, requestId, requestData } }));
    });
  }

  async call<R extends VendorRequest>(request: R, data?: Req<R>): Promise<Res<R>> {
    const r = await this.#request('CallVendorRequest', { vendorName: VENDOR_NAME, requestType: request, requestData: data ?? {} });
    if (!r.requestStatus.result) {
      if (r.requestStatus.code === NOT_FOUND) {
        const why = r.requestStatus.comment ?? '';
        if (why && !/no vendor/i.test(why)) throw new HostError('unsupported', why);
        this.conn = 'no_stage';
        throw new HostError('no_stage');
      }
      throw new HostError('internal', r.requestStatus.comment);
    }
    // obs-websocket wraps the vendor answer one level deeper (vendor-api.md 1).
    return (r.responseData?.responseData ?? { ok: false, code: 'internal', error: 'empty' }) as Res<R>;
  }

  on<E extends keyof HostEvents>(event: E, fn: (d: HostEvents[E]) => void): () => void {
    return this.#em.on(event, fn);
  }

  retry(): void {
    if (this.#identified) {
      this.conn = 'connecting';
      void this.#probe();
    } else this.connect();
  }
}
