#!/usr/bin/env node
// Checks the presets Parallaxa ships (data/presets) and keeps the catalogue of
// free preset ids that clients read (docs/protocol/builtin-presets.json):
//   - moves/recipes/fx/*.json validate against docs/protocol/schemas
//     (move.json, recipe.json, fx-preset.json); file name = "<id>.json";
//   - moves: key times strictly increase and end at t = 1; every built-in move
//     (vendor-tables.json "moves") has a file; at least 6 singing moves;
//   - recipes: the looks clients rely on exist; every fx reference resolves;
//   - fx: every LUT reference has luts/<lut>.png; neutral has empty params;
//   - luts: 512x512 PNGs (64^3 strip layout); identity.png is exact;
//   - samples/parallaxa_sample.json: images shipped, the three filters used,
//     layer roles bg,char,fg from bottom to top;
//   - docs/protocol/builtin-presets.json equals what data/presets holds.
//
//   node tests/contracts/presets_check.mjs           check (exit 1 on any problem)
//   node tests/contracts/presets_check.mjs --write   rewrite docs/protocol/builtin-presets.json
//
// Copyright (C) 2026 Parallaxa Labs
// SPDX-License-Identifier: GPL-2.0-or-later
import { existsSync, readdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { inflateSync } from 'node:zlib';
import { loadSchemas } from './schema.mjs';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const PRESETS = join(ROOT, 'data', 'presets');
const CATALOG = join(ROOT, 'docs', 'protocol', 'builtin-presets.json');
const readJSON = (p) => JSON.parse(readFileSync(p, 'utf8'));

// Ids that AI Live Studio, the docks and the UX spec refer to by name.
const REQUIRED_RECIPES = ['clean', 'cozy_chat', 'talk_show', 'singing_ballad', 'singing_hype', 'gaming_facecam', 'late_night_lofi',
  'spotlight', 'product_showcase', 'brb', 'karaoke_spotlight', 'rainy_lofi', 'hype_raid', 'horror'];
const REQUIRED_FX = ['neutral', 'cozy', 'film', 'dreamy', 'horror', 'hype'];
const REQUIRED_LUTS = ['identity', 'film', 'night'];
const SAMPLE_FILTERS = ['stage_camera_rig', 'stage_fx', 'stage_depth_blur'];

const errs = [];
const err = (m) => errs.push(m);

/** Minimal PNG reader: 8-bit RGB or RGBA, non-interlaced. Returns {width, height, channels, px}. */
export function readPng(buf) {
  if (buf.readUInt32BE(0) !== 0x89504e47) throw new Error('not a PNG');
  let off = 8;
  let width = 0, height = 0, depth = 0, color = 0, interlace = 0;
  const idat = [];
  while (off < buf.length) {
    const len = buf.readUInt32BE(off);
    const type = buf.toString('latin1', off + 4, off + 8);
    const data = buf.subarray(off + 8, off + 8 + len);
    if (type === 'IHDR') {
      width = data.readUInt32BE(0); height = data.readUInt32BE(4);
      depth = data[8]; color = data[9]; interlace = data[12];
    } else if (type === 'IDAT') idat.push(data);
    else if (type === 'IEND') break;
    off += 12 + len;
  }
  const channels = { 2: 3, 6: 4 }[color];
  if (depth !== 8 || !channels || interlace) throw new Error(`unsupported PNG (depth ${depth}, color type ${color}, interlace ${interlace})`);
  const raw = inflateSync(Buffer.concat(idat));
  const stride = width * channels;
  const px = Buffer.alloc(stride * height);
  for (let y = 0; y < height; y++) {
    const f = raw[y * (stride + 1)];
    const src = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1));
    for (let i = 0; i < stride; i++) {
      const a = i >= channels ? px[y * stride + i - channels] : 0;
      const b = y > 0 ? px[(y - 1) * stride + i] : 0;
      const c = y > 0 && i >= channels ? px[(y - 1) * stride + i - channels] : 0;
      let pred = 0;
      if (f === 1) pred = a;
      else if (f === 2) pred = b;
      else if (f === 3) pred = (a + b) >> 1;
      else if (f === 4) {
        const p = a + b - c, pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
        pred = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
      } else if (f !== 0) throw new Error(`bad filter ${f} on row ${y}`);
      px[y * stride + i] = (src[i] + pred) & 0xff;
    }
  }
  return { width, height, channels, px };
}

/** Pixel errors of a 64^3 identity strip LUT (r = x % 64, g = y % 64, b = tile). */
export function identityErrors(png) {
  const q = (v) => Math.round((v * 255) / 63);
  for (let y = 0; y < png.height; y++)
    for (let x = 0; x < png.width; x++) {
      const i = (y * png.width + x) * png.channels;
      const want = [q(x % 64), q(y % 64), q(Math.floor(y / 64) * 8 + Math.floor(x / 64))];
      if (png.px[i] !== want[0] || png.px[i + 1] !== want[1] || png.px[i + 2] !== want[2])
        return [`identity (${x},${y}) = ${[...png.px.subarray(i, i + 3)]}, want ${want}`];
    }
  return [];
}

const list = (sub, ext) =>
  existsSync(join(PRESETS, sub)) ? readdirSync(join(PRESETS, sub)).filter((f) => f.endsWith(ext)).sort() : [];

const validate = loadSchemas(join(ROOT, 'docs', 'protocol', 'schemas'));
const tables = readJSON(join(ROOT, 'docs', 'protocol', 'vendor-tables.json'));
const files = { moves: {}, recipes: {}, fx: {} };

for (const [sub, schema] of [['moves', 'move.json'], ['recipes', 'recipe.json'], ['fx', 'fx-preset.json']]) {
  for (const f of list(sub, '.json')) {
    const where = `${sub}/${f}`;
    let v;
    try {
      v = readJSON(join(PRESETS, sub, f));
    } catch (e) {
      err(`${where}: ${e.message}`);
      continue;
    }
    for (const e of validate(schema, v, where)) err(e);
    if (f !== `${v.id}.json`) err(`${where}: file name must be ${v.id}.json`);
    files[sub][v.id] = v;
  }
}

// moves
for (const [id, m] of Object.entries(files.moves)) {
  const keys = m.keys ?? [];
  for (let i = 1; i < keys.length; i++) if (!(keys[i].t > keys[i - 1].t)) err(`moves/${id}: key t must strictly increase`);
  if (keys.length && keys[keys.length - 1].t !== 1) err(`moves/${id}: last key must be t = 1`);
}
for (const m of tables.tables.moves) if (!files.moves[m]) err(`built-in move ${m} has no preset file`);
const singing = Object.values(files.moves).filter((m) => JSON.stringify(m).includes('"singing"')).length;
if (singing < 6) err(`${singing} singing moves, want at least 6`);

// recipes and fx
for (const id of REQUIRED_RECIPES) if (!files.recipes[id]) err(`recipe ${id} missing`);
for (const id of REQUIRED_FX) if (!files.fx[id]) err(`fx preset ${id} missing`);
for (const [id, r] of Object.entries(files.recipes)) if (r.fx && !files.fx[r.fx]) err(`recipes/${id}: unknown fx "${r.fx}"`);
for (const [id, p] of Object.entries(files.fx)) {
  const lut = p.params?.lut;
  if (lut && !existsSync(join(PRESETS, 'luts', `${lut}.png`))) err(`fx/${id}: LUT "${lut}" has no luts/${lut}.png`);
}
if (files.fx.neutral && Object.keys(files.fx.neutral.params ?? {}).length) err('fx/neutral.json must have empty (all-neutral) params');

// LUTs
const luts = list('luts', '.png').map((f) => f.slice(0, -4));
for (const id of REQUIRED_LUTS) if (!luts.includes(id)) err(`luts/${id}.png missing`);
for (const id of luts) {
  try {
    const png = readPng(readFileSync(join(PRESETS, 'luts', `${id}.png`)));
    if (png.width !== 512 || png.height !== 512) err(`luts/${id}.png is ${png.width}x${png.height}, want 512x512`);
    else if (id === 'identity') for (const e of identityErrors(png)) err(e);
  } catch (e) {
    err(`luts/${id}.png: ${e.message}`);
  }
}

// sample scene collection
const sampleDir = join(PRESETS, 'samples');
const samplePath = join(sampleDir, 'parallaxa_sample.json');
if (!existsSync(samplePath)) err('samples/parallaxa_sample.json missing');
else {
  const col = readJSON(samplePath);
  const uuids = new Set();
  const filters = new Set();
  let scene = null;
  for (const s of col.sources ?? []) {
    uuids.add(s.uuid);
    for (const f of s.filters ?? []) filters.add(f.id);
    if (s.id === 'scene') scene = s.settings;
    if (s.id === 'image_source' && !existsSync(join(sampleDir, s.settings?.file ?? ''))) err(`sample ${s.name}: image "${s.settings?.file}" not shipped`);
  }
  for (const f of SAMPLE_FILTERS) if (!filters.has(f)) err(`sample misses filter ${f}`);
  const roles = [];
  for (const it of scene?.items ?? []) {
    if (!uuids.has(it.source_uuid)) err(`sample item ${it.name} references an unknown source`);
    roles.push(it.private_settings?.['stage.role']);
  }
  if (roles.join(',') !== 'bg,char,fg') err(`sample layer roles ${roles}, want bg,char,fg (bottom to top)`);
}

// catalogue for clients (AI Live Studio vendors docs/protocol)
const sortedKeys = (o) => Object.keys(o).sort();
const catalog = {
  $comment: 'Generated by tests/contracts/presets_check.mjs --write from data/presets. The presets the free Parallaxa ships: recipe and move ids, FX presets with their params, LUT ids.',
  recipes: sortedKeys(files.recipes),
  moves: sortedKeys(files.moves),
  fx: Object.fromEntries(sortedKeys(files.fx).map((id) => [id, files.fx[id].params ?? {}])),
  luts,
};
const text = JSON.stringify(catalog, null, 2) + '\n';
if (process.argv.includes('--write')) {
  writeFileSync(CATALOG, text);
  console.log(`presets_check: wrote ${CATALOG}`);
} else if (!existsSync(CATALOG) || readFileSync(CATALOG, 'utf8').replace(/\r\n/g, '\n') !== text) {
  err('docs/protocol/builtin-presets.json is stale (node tests/contracts/presets_check.mjs --write)');
}

for (const e of errs) console.error(e);
const n = Object.values(files).reduce((s, o) => s + Object.keys(o).length, 0);
console.log(errs.length ? `presets_check: ${errs.length} problem(s)` : `presets_check: ${n} presets and ${luts.length} LUTs ok`);
process.exit(errs.length ? 1 : 0);
