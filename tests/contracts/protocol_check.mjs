#!/usr/bin/env node
// Checks include/stage-protocol.h (the C mirror of the vendor API) against
// the normative protocol data in docs/protocol:
//   - every constant of names.vendor.json (vendor groups and the untyped
//     filter / vendor names) appears in the header as a "quoted" string;
//   - the ordered name tables (stage_*_names) equal vendor-tables.json
//     "tables", in order;
//   - STAGE_PROTOCOL_VERSION and every numeric limit equal vendor-tables.json
//     "protocol" / "limits" (key "depth_min" -> STAGE_DEPTH_MIN);
//   - the C id/name buffers fit vendor-tables.json "buffers".
// names.vendor.json and vendor-tables.json are generated from the reference
// client implementation and copied here (docs/protocol/README.md).
//
//   node tests/contracts/protocol_check.mjs             exit 1 on any mismatch
//   node tests/contracts/protocol_check.mjs --selftest  plant one mismatch per rule, expect each caught
//
// Copyright (C) 2026 Parallaxa Labs
// SPDX-License-Identifier: GPL-2.0-or-later
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const read = (p) => readFileSync(join(ROOT, p), 'utf8').replace(/\r\n/g, '\n');

// names.vendor.json groups (and untyped constants) that belong to the vendor API.
export const VENDOR_GROUPS = [
  'VendorRequest', 'VendorEvent', 'VendorError', 'Feature', 'Move', 'Ease', 'AutoMode',
  'Role', 'Transition', 'PulseEffect', 'HotkeyForwardAction', 'PerfPaused',
];
export const VENDOR_UNTYPED = ['VendorName', 'FilterCameraRig', 'FilterFX', 'FilterDepthBlur', 'DepthBlurKeyAutoStrength'];

// vendor-tables.json "tables" key -> C table name.
export const C_TABLES = {
  requests: 'stage_request_names',
  moves: 'stage_move_names',
  eases: 'stage_ease_names',
  auto_modes: 'stage_auto_mode_names',
  roles: 'stage_role_names',
  transitions: 'stage_transition_names',
  pulse_effects: 'stage_pulse_names',
  hotkey_forward_actions: 'stage_hotkey_action_names',
  errors: 'stage_error_names',
};

/** "#define NAME value" lines of the header (trailing comments dropped). */
export function cDefines(src) {
  const out = {};
  for (const m of src.matchAll(/^#define\s+(\w+)\s+(.+?)\s*(?:\/\*.*)?$/gm)) out[m[1]] = m[2].trim();
  return out;
}

/** Numeric value of a macro ("(0.1)", "32u", "2.0f" style), or NaN. */
export function cNumber(defs, name) {
  const v = defs[name];
  if (v === undefined) return NaN;
  return Number(v.replace(/^\(+|\)+$/g, '').replace(/[uUfF]$/, ''));
}

/** `static const char *const <name>[...] = { ... };` with macro names resolved, or null. */
export function cTable(src, defs, name) {
  const m = src.match(new RegExp(`static const char \\*const ${name}\\[[^\\]]*\\]\\s*=\\s*\\{([\\s\\S]*?)\\};`));
  if (!m) return null;
  return m[1]
    .split(',')
    .map((tok) => tok.trim())
    .filter(Boolean)
    .map((tok) => {
      const v = defs[tok] ?? tok;
      return /^".*"$/.test(v) ? JSON.parse(v) : `<${tok}>`;
    });
}

/** Every mismatch between the header text and the protocol data. */
export function check(header, names, tables) {
  const errs = [];
  const defs = cDefines(header);
  const quoted = (v) => header.includes(`"${v}"`);
  for (const g of VENDOR_GROUPS) {
    if (!names[g]) { errs.push(`names.vendor.json: group ${g} missing`); continue; }
    for (const [k, v] of Object.entries(names[g])) if (!quoted(v)) errs.push(`stage-protocol.h: ${g}.${k} = "${v}" not found`);
  }
  for (const k of VENDOR_UNTYPED) {
    const v = names.untyped?.[k];
    if (v === undefined) errs.push(`names.vendor.json: untyped.${k} missing`);
    else if (!quoted(v)) errs.push(`stage-protocol.h: ${k} = "${v}" not found`);
  }
  for (const [key, want] of Object.entries(tables.tables)) {
    const cName = C_TABLES[key];
    if (!cName) { errs.push(`vendor-tables.json: table ${key} has no C mirror in protocol_check.mjs`); continue; }
    const got = cTable(header, defs, cName);
    if (!got) errs.push(`stage-protocol.h: table ${cName} missing`);
    else if (got.join(',') !== want.join(',')) errs.push(`stage-protocol.h: ${cName} = [${got}], protocol = [${want}]`);
  }
  if (cNumber(defs, 'STAGE_PROTOCOL_VERSION') !== tables.protocol)
    errs.push(`STAGE_PROTOCOL_VERSION = ${defs.STAGE_PROTOCOL_VERSION}, protocol = ${tables.protocol}`);
  for (const [key, want] of Object.entries(tables.limits)) {
    const macro = `STAGE_${key.toUpperCase()}`;
    const got = cNumber(defs, macro);
    if (!(Math.abs(got - want) <= 1e-12)) errs.push(`${macro} = ${defs[macro] ?? 'missing'}, protocol = ${want}`);
  }
  if (cNumber(defs, 'STAGE_ID_MAX') !== tables.buffers.id_max_len + 1)
    errs.push(`STAGE_ID_MAX = ${defs.STAGE_ID_MAX}, want id_max_len + 1 = ${tables.buffers.id_max_len + 1}`);
  if (cNumber(defs, 'STAGE_VIEW_NAME_MAX') !== tables.buffers.view_name_bytes)
    errs.push(`STAGE_VIEW_NAME_MAX = ${defs.STAGE_VIEW_NAME_MAX}, want ${tables.buffers.view_name_bytes}`);
  return errs;
}

/** Plants one mismatch per rule and returns the rules that were not caught. */
function selftest(header, names, tables) {
  const clone = (x) => JSON.parse(JSON.stringify(x));
  const cases = {
    'vendor name missing from the header': () => {
      const n = clone(names);
      n.VendorRequest.__planted = 'planted_request';
      return [header, n, tables];
    },
    'table order differs': () => {
      const t = clone(tables);
      t.tables.moves = [...t.tables.moves].reverse();
      return [header, names, t];
    },
    'limit differs': () => {
      const t = clone(tables);
      t.limits.zoom_max += 1;
      return [header, names, t];
    },
    'protocol version differs': () => {
      const t = clone(tables);
      t.protocol += 1;
      return [header, names, t];
    },
    'buffer size differs': () => {
      const t = clone(tables);
      t.buffers.view_name_bytes += 8;
      return [header, names, t];
    },
    'C table missing': () => [header.replace(/stage_pulse_names/g, 'stage_pulse_names_x'), names, tables],
  };
  const missed = [];
  for (const [rule, plant] of Object.entries(cases)) if (check(...plant()).length === 0) missed.push(rule);
  return missed;
}

const header = read('include/stage-protocol.h');
const names = JSON.parse(read('docs/protocol/names.vendor.json'));
const tables = JSON.parse(read('docs/protocol/vendor-tables.json'));
const base = check(header, names, tables);
if (process.argv.includes('--selftest')) {
  if (base.length) {
    console.error('selftest needs a clean tree first:\n  ' + base.join('\n  '));
    process.exit(1);
  }
  const missed = selftest(header, names, tables);
  for (const m of missed) console.error(`selftest: planted "${m}" was not caught`);
  console.log(`protocol_check selftest: ${6 - missed.length}/6 rules caught their planted mismatch`);
  process.exit(missed.length ? 1 : 0);
}
for (const e of base) console.error(e);
const nTables = Object.keys(tables.tables).length;
const nLimits = Object.keys(tables.limits).length;
console.log(base.length
  ? `protocol_check: ${base.length} mismatch(es) between include/stage-protocol.h and docs/protocol`
  : `protocol_check: stage-protocol.h matches docs/protocol (${VENDOR_GROUPS.length} name groups, ${nTables} tables, ${nLimits} limits)`);
process.exit(base.length ? 1 : 0);
