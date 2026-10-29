#!/usr/bin/env node
// smoke.mjs — live check of the Parallaxa obs-websocket vendor
// (docs/protocol/vendor-api.md) against a running OBS. Node 22+ (built-in
// WebSocket and crypto), no dependencies.
//
//   node tests/vendor-smoke/smoke.mjs [--url ws://127.0.0.1:4455]
//        [--password <pw>] [--scene "<scene with a Parallaxa Camera Rig>"]
//        [--soak <seconds>]
//
// What it does (on the current program scene unless --scene is given):
//   1. every request with a valid payload; each responseData is validated
//      against docs/protocol/schemas/vendor.responses.json;
//   2. invalid payloads; the documented error codes are expected;
//   3. events: state_changed <= 10/s while the camera moves and silence once
//      it is idle (auto camera is switched off for this part), move_finished
//      for a camera_move, schema-checked;
//   4. --soak N: camera_move at 20 req/s for N seconds (acceptance: 600),
//      reporting errors and round-trip latency.
// It moves the camera, switches auto camera off (restored at the end) and
// uses view slot 9 only when that slot is empty (deleted afterwards).
//
// obs-websocket converts vendor replies through obs_data, which has no null
// and no arrays of strings (vendor-api.md 1.1): the contract makes
// get_info.features and recipe_list[].tags optional, and layer_list sends
// "depth" exactly for enrolled items. Responses must match the schemas exactly.
//
// Copyright (C) 2026 Parallaxa Labs
// SPDX-License-Identifier: GPL-2.0-or-later

import { createHash } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadSchemas } from '../contracts/schema.mjs';

const args = Object.fromEntries(
  process.argv.slice(2).reduce((acc, a, i, all) => {
    if (a.startsWith('--')) acc.push([a.slice(2), all[i + 1]?.startsWith('--') ? true : all[i + 1] ?? true]);
    return acc;
  }, []),
);
const URL_ = args.url || 'ws://127.0.0.1:4455';
const VENDOR = 'parallaxa';
const schemaDir = join(dirname(fileURLToPath(import.meta.url)), '../../docs/protocol/schemas');

// JSON Schema subset validator shared with tests/contracts (presets_check.mjs).
const validate = loadSchemas(schemaDir);

function checkSchema(kind, name, value) {
  const file = kind === 'event' ? 'vendor.events.json' : 'vendor.responses.json';
  const errs = validate(`${file}#/$defs/${name}`, value, name);
  // Rule the schema subset cannot state (vendor-api.md 4.10): depth is present
  // exactly when the item is enrolled.
  if (name === 'layer_list' && Array.isArray(value?.layers))
    value.layers.forEach((l, i) => {
      if ('depth' in l !== (l.role !== 'none')) errs.push(`layer_list.layers[${i}]: depth ${'depth' in l ? 'present' : 'absent'} with role ${l.role}`);
    });
  return errs;
}

// ---------------------------------------------------------------------------
// --validate <file>: offline mode used by ctest (vendor_schema_check). The
// file holds one JSON object per line written by vendor_test:
// {"response": "<requestType>", "data": {...}} or {"event": "<type>", "data": {...}}.
// ---------------------------------------------------------------------------
if (args.validate) {
  let n = 0, bad = 0;
  for (const line of readFileSync(args.validate, 'utf8').split(String.fromCharCode(10))) {
    if (!line.trim()) continue;
    const rec = JSON.parse(line);
    const errs = rec.event
      ? checkSchema('event', rec.event, rec.data)
      : checkSchema('response', rec.data.ok === false ? 'error' : rec.response, rec.data);
    n++;
    if (errs.length) {
      bad++;
      console.log(`  FAIL ${rec.event || rec.response}: ${errs.join('; ')}`);
    }
  }
  console.log(`validated ${n} payloads against docs/protocol/schemas, ${bad} invalid`);
  process.exit(bad || !n ? 1 : 0);
}

// ---------------------------------------------------------------------------
// obs-websocket v5 client
// ---------------------------------------------------------------------------
const sha = (s) => createHash('sha256').update(s).digest('base64');
const ws = new WebSocket(URL_, 'obswebsocket.json');
const pending = new Map();
const events = [];
let nextId = 1;

function send(op, d) {
  ws.send(JSON.stringify({ op, d }));
}

const ready = new Promise((resolve, reject) => {
  ws.onerror = () => reject(new Error(`cannot connect to ${URL_} (is obs-websocket enabled?)`));
  ws.onmessage = (m) => {
    const { op, d } = JSON.parse(m.data);
    if (op === 0) {
      const ident = { rpcVersion: 1, eventSubscriptions: 1 << 9 };
      if (d.authentication) {
        if (!args.password) return reject(new Error('the server needs --password'));
        ident.authentication = sha(sha(args.password + d.authentication.salt) + d.authentication.challenge);
      }
      send(1, ident);
    } else if (op === 2) resolve();
    else if (op === 7) pending.get(d.requestId)?.(d);
    else if (op === 5 && d.eventType === 'VendorEvent' && d.eventData.vendorName === VENDOR)
      events.push({ t: Date.now(), type: d.eventData.eventType, data: d.eventData.eventData });
  };
  ws.onclose = (e) => reject(new Error(`closed: ${e.code} ${e.reason}`));
});

function call(requestType, requestData = {}) {
  const requestId = String(nextId++);
  const t0 = performance.now();
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`${requestType}: timeout`)), 5000);
    pending.set(requestId, (d) => {
      clearTimeout(timer);
      pending.delete(requestId);
      if (!d.requestStatus.result) return reject(new Error(`${requestType}: ${d.requestStatus.code} ${d.requestStatus.comment ?? ''}`));
      resolve({ res: d.responseData.responseData, ms: performance.now() - t0 });
    });
    send(6, { requestType: 'CallVendorRequest', requestId, requestData: { vendorName: VENDOR, requestType, requestData } });
  });
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let failures = 0;
function fail(msg) {
  failures++;
  console.log(`  FAIL ${msg}`);
}

async function expectOk(type, data, schemaName = type) {
  const { res } = await call(type, data);
  if (!res.ok) fail(`${type} ${JSON.stringify(data)} -> ${JSON.stringify(res)}`);
  else for (const e of checkSchema('response', schemaName, res)) fail(e);
  return res;
}

async function expectErr(type, data, code) {
  const { res } = await call(type, data);
  if (res.ok || res.code !== code) fail(`${type} ${JSON.stringify(data)}: want ${code}, got ${JSON.stringify(res)}`);
  else for (const e of checkSchema('response', 'error', res)) fail(e);
}

// ---------------------------------------------------------------------------
// Test run
// ---------------------------------------------------------------------------
try {
  await ready;
} catch (e) {
  console.log(`smoke: ${e.message}`);
  process.exit(2);
}
console.log(`connected to ${URL_}`);
const S = args.scene ? { scene: args.scene } : {};

const info = await expectOk('get_info');
console.log(`  Parallaxa ${info.version}, protocol ${info.protocol}, OBS ${info.obs}, ${info.rigs.length} rig(s), ${info.fx.length} FX`);
const st0 = await expectOk('get_state', S);
const hasFx = info.fx.some((f) => f.source_uuid === st0.scene_uuid);

console.log('requests');
await expectOk('camera_move', { ...S, move: 'zoom_in', duration_ms: 800, intensity: 1.2, ease: 'ease_out_cubic' });
await expectOk('camera_set', { ...S, x: 40, y: -20, zoom: 1.2, rot: 0, duration_ms: 300 });
await expectOk('camera_set', { ...S, x: -10, zoom: -0.05, relative: true });
await expectOk('camera_reset', { ...S, duration_ms: 200 });
await expectOk('camera_auto', { ...S, mode: st0.auto.mode, intensity: st0.auto.intensity });
await expectOk('dof_set', { ...S, focus_depth: st0.dof.focus_depth, amount: st0.dof.amount, duration_ms: 0 });
const layers = await expectOk('layer_list', S);
await expectOk('move_list');
await expectOk('recipe_list');
if (hasFx) {
  await expectOk('fx_set', { ...S, preset: st0.fx.preset || 'neutral', duration_ms: 0 });
  await expectOk('fx_pulse', { ...S, effect: 'vignette_pulse', intensity: 0.3, duration_ms: 200 });
} else console.log('  (no Parallaxa FX on this scene: fx_set / fx_pulse expect no_fx)');
const views = await expectOk('view_list', S);
if (!views.views.some((v) => v.slot === 9)) {
  const saved = await expectOk('view_save', { ...S, slot: 9, name: 'smoke', transition: 'cut' });
  const kept = await expectOk('view_save', { ...S, slot: 9, name: 'smoke 2', keep_pose: true });
  if (JSON.stringify(kept.view.camera) !== JSON.stringify(saved.view.camera) || kept.view.transition !== 'cut')
    fail('view_save keep_pose changed the stored pose or transition');
  await expectOk('view_apply', { ...S, slot: 9 }, 'cmd');
  await expectOk('view_delete', { ...S, slot: 9 });
}
const fr = await expectOk('freeze', { ...S, on: st0.frozen });
if (fr.frozen !== st0.frozen) fail('freeze did not echo the requested value');
if (layers.layers.length) {
  const l = layers.layers.find((x) => x.role !== 'none' && 'depth' in x);
  if (l) await expectOk('layer_set', { ...S, item_id: l.item_id, depth: l.depth, role: l.role });
}

console.log('errors');
await expectErr('camera_move', { ...S, move: 'spin' }, 'not_found');
await expectErr('camera_move', { ...S, move: 'zoom_in', duration_ms: 10 }, 'bad_param');
await expectErr('camera_move', { ...S, move: 'zoom_in', bogus: 1 }, 'bad_param');
await expectErr('camera_set', { ...S, zoom: 9 }, 'bad_param');
await expectErr('camera_auto', { ...S, mode: 'spin' }, 'bad_param');
await expectErr('get_state', { scene: 'no such scene 1234' }, 'not_found');
await expectErr('recipe_apply', { ...S, recipe: 'no_such_recipe' }, 'not_found');
await expectErr('view_apply', { ...S, slot: 0 }, 'bad_param');
await expectErr('layer_set', { ...S, item_id: 987654321, depth: 1 }, 'not_found');
await expectErr('fx_set', { ...S, params: { vignette: 3 } }, 'bad_param');
await expectErr(hasFx ? 'fx_set' : 'fx_pulse', hasFx ? { ...S, preset: 'no_such_fx' } : { ...S, effect: 'flash' }, hasFx ? 'not_found' : 'no_fx');
await expectErr('panic', { x: 1 }, 'bad_param');

console.log('events');
await call('camera_auto', { ...S, mode: 'off' });
await sleep(1500);
events.length = 0;
const mv = await expectOk('camera_move', { ...S, move: 'pan_left', duration_ms: 2000 });
for (let i = 0; i < 20; i++) {
  await call('camera_set', { ...S, x: 2, relative: true, duration_ms: 0 });
  await sleep(50);
}
await sleep(2500);
const t1 = Date.now();
await sleep(3000);
const sc = events.filter((e) => e.type === 'state_changed' && e.data.scene_uuid === st0.scene_uuid);
const perSec = new Map();
for (const e of sc) perSec.set(Math.floor(e.t / 1000), (perSec.get(Math.floor(e.t / 1000)) || 0) + 1);
const maxRate = Math.max(0, ...perSec.values());
console.log(`  state_changed: ${sc.length} events, max ${maxRate} in one second`);
if (maxRate > 11) fail(`state_changed rate ${maxRate}/s > 10/s`);
if (sc.some((e) => e.t > t1)) fail('state_changed kept coming while the camera was idle');
const mf = events.find((e) => e.type === 'move_finished' && e.data.id === mv.id);
if (!mf) fail(`no move_finished for id ${mv.id}`);
for (const e of events)
  for (const err of checkSchema('event', e.type, e.data)) fail(`event ${err}`);

if (args.soak) {
  const secs = Number(args.soak);
  console.log(`soak: camera_move at 20 req/s for ${secs} s`);
  const lat = [];
  let errors = 0;
  const end = Date.now() + secs * 1000;
  let n = 0;
  while (Date.now() < end) {
    const tick = Date.now();
    call('camera_move', { ...S, move: n++ % 2 ? 'zoom_out' : 'zoom_in', duration_ms: 300 })
      .then(({ res, ms }) => (res.ok ? lat.push(ms) : errors++))
      .catch(() => errors++);
    await sleep(Math.max(0, 50 - (Date.now() - tick)));
  }
  await sleep(1000);
  lat.sort((a, b) => a - b);
  const p = (q) => (lat.length ? lat[Math.floor(q * (lat.length - 1))].toFixed(2) : '-');
  console.log(`  ${n} requests, ${lat.length} ok, ${errors} errors; latency p50 ${p(0.5)} ms, p99 ${p(0.99)} ms`);
  if (errors) fail(`${errors} soak errors`);
  await expectOk('get_info');
}

await call('camera_reset', { ...S, duration_ms: 300 });
await call('camera_auto', { ...S, mode: st0.auto.mode, intensity: st0.auto.intensity });
console.log(failures ? `smoke: ${failures} FAILURE(S)` : 'smoke: OK');
ws.close();
process.exit(failures ? 1 : 0);
