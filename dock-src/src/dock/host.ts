// Copyright (C) 2026 Parallaxa Labs
// SPDX-License-Identifier: GPL-2.0-or-later
// What the Stage control UI needs from its transport. The standalone dock's
// host is obs-host.svelte.ts (Parallaxa data/dock/dock-standalone.html), which
// talks to obs-websocket v5 directly (vendor-api.md 1); AI Live Studio has its
// own host that relays the same vendor requests. The UI (StageControl.svelte
// and the model) only sees this interface.
import type { VendorRequest, VendorRequests, VendorResponse } from '../lib/protocol/vendor.ts';

export type Lang = 'en' | 'ja' | 'id';

/**
 * Connection to Stage, as the UI explains it:
 * connecting  looking for Stage
 * offline     the host itself is down (a relaying host lost its own connection)
 * obs_down    OBS not reachable (reported by a relaying host)
 * ws_off      OBS WebSocket server off, or OBS in Safe Mode
 * auth        obs-websocket needs a password, or the stored one is wrong (standalone)
 * no_stage    OBS answers but the `parallaxa` vendor does not
 * ready       requests can be sent
 */
export type Conn = 'connecting' | 'offline' | 'obs_down' | 'ws_off' | 'auth' | 'no_stage' | 'ready';

/** Vendor events (vendor-api.md 7) plus `program_scene` (OBS switched its program scene). */
export interface HostEvents {
  state_changed: VendorRequests['get_state']['res'];
  move_finished: { scene: string; scene_uuid: string; id: number; move: string; cancelled: boolean };
  rig_changed: { scene: string; scene_uuid: string; enabled: boolean };
  perf_warning: { paused: string; filter: string; cost_ms: number };
  program_scene: { scene: string };
}

export type Req<R extends VendorRequest> = VendorRequests[R]['req'];
export type Res<R extends VendorRequest> = VendorResponse<VendorRequests[R]['res']>;

/**
 * A transport failure (no vendor payload): `code` is a UI error code, `disconnected`,
 * `no_stage`, or `unsupported` (Stage is there but older: it lacks this request).
 */
export class HostError extends Error {
  constructor(
    readonly code: string,
    msg = code,
  ) {
    super(msg);
  }
}

export interface StageHost {
  /** Reactive connection state. */
  readonly conn: Conn;
  /** Reactive: OBS is streaming (on-air lock). */
  readonly streaming: boolean;
  /** Reactive: bumped on every (re)connection that reached Stage; the UI reloads its lists. */
  readonly epoch: number;
  /** Reactive UI language (localized move and recipe names). */
  readonly lang: Lang;
  /** Reactive: the UI runs with reduced motion. */
  readonly reduceMotion: boolean;
  setReduceMotion(on: boolean): void;
  /** Target scene saved by the host ('' = current program scene). */
  readonly scene: string;
  setScene(scene: string): void;
  /**
   * Sends one vendor request. Resolves with the vendor payload, `ok:false`
   * included (vendor-api.md 8); rejects with HostError when nothing answered.
   */
  call<R extends VendorRequest>(request: R, data?: Req<R>): Promise<Res<R>>;
  on<E extends keyof HostEvents>(event: E, fn: (d: HostEvents[E]) => void): () => void;
  /** Asks the host to look for Stage again ("I installed it, check again"). */
  retry(): void;
}

/** Tiny listener registry shared by both hosts. */
export class Emitter {
  #fns = new Map<string, Set<(d: never) => void>>();
  on<E extends keyof HostEvents>(event: E, fn: (d: HostEvents[E]) => void): () => void {
    let s = this.#fns.get(event);
    if (!s) this.#fns.set(event, (s = new Set()));
    s.add(fn as (d: never) => void);
    return () => s.delete(fn as (d: never) => void);
  }
  emit<E extends keyof HostEvents>(event: E, d: HostEvents[E]): void {
    for (const fn of [...(this.#fns.get(event) ?? [])]) {
      try {
        (fn as (d: HostEvents[E]) => void)(d);
      } catch (e) {
        console.error('[stage] listener failed', e);
      }
    }
  }
}
