// Minimal JSON Schema validator for the 2020-12 subset used by
// docs/protocol/schemas ($ref across files, type, const, enum, numeric and
// string bounds, pattern, required, properties, additionalProperties: false,
// items, min/maxItems, minProperties, anyOf, oneOf). No dependencies.
//
// Copyright (C) 2026 Parallaxa Labs
// SPDX-License-Identifier: GPL-2.0-or-later
import { readdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';

/** Loads every *.json schema of a folder; returns a validator bound to them. */
export function loadSchemas(dir) {
  const schemas = {};
  for (const f of readdirSync(dir).filter((n) => n.endsWith('.json'))) schemas[f] = JSON.parse(readFileSync(join(dir, f), 'utf8'));

  function resolveRef(ref, file) {
    const [f, ptr = ''] = ref.split('#');
    const name = f || file;
    const doc = schemas[name];
    if (!doc) throw new Error(`$ref ${ref}: unknown schema file ${name}`);
    const node = ptr.split('/').slice(1).reduce((n, k) => n?.[k], doc);
    if (!node) throw new Error(`$ref ${ref}: not found`);
    return [node, name];
  }

  function typeOf(v) {
    return v === null ? 'null' : Array.isArray(v) ? 'array' : typeof v;
  }

  function typeOk(want, v, t) {
    if (Array.isArray(want)) return want.some((w) => typeOk(w, v, t));
    return want === t || (want === 'integer' && Number.isInteger(v));
  }

  function check(v, s, file, path, errs) {
    if (s.$ref) {
      const [node, f] = resolveRef(s.$ref, file);
      check(v, node, f, path, errs);
    }
    const t = typeOf(v);
    if ('const' in s && v !== s.const) errs.push(`${path}: want ${JSON.stringify(s.const)}`);
    if (s.enum && !s.enum.includes(v)) errs.push(`${path}: ${JSON.stringify(v)} not in enum`);
    if (s.type && !typeOk(s.type, v, t)) errs.push(`${path}: type ${t}, want ${s.type}`);
    if (t === 'number') {
      if (s.minimum !== undefined && v < s.minimum) errs.push(`${path}: ${v} < ${s.minimum}`);
      if (s.maximum !== undefined && v > s.maximum) errs.push(`${path}: ${v} > ${s.maximum}`);
    }
    if (t === 'string') {
      const n = [...v].length;
      if (s.maxLength !== undefined && n > s.maxLength) errs.push(`${path}: longer than ${s.maxLength}`);
      if (s.minLength !== undefined && n < s.minLength) errs.push(`${path}: shorter than ${s.minLength}`);
      if (s.pattern && !new RegExp(s.pattern, 'u').test(v)) errs.push(`${path}: does not match ${s.pattern}`);
    }
    if (t === 'object') {
      for (const k of s.required || []) if (!(k in v)) errs.push(`${path}: missing ${k}`);
      if (s.minProperties !== undefined && Object.keys(v).length < s.minProperties) errs.push(`${path}: fewer than ${s.minProperties} properties`);
      for (const [k, val] of Object.entries(v)) {
        if (s.properties?.[k]) check(val, s.properties[k], file, `${path}.${k}`, errs);
        else if (s.additionalProperties === false) errs.push(`${path}: unexpected ${k}`);
        else if (s.additionalProperties && typeof s.additionalProperties === 'object') check(val, s.additionalProperties, file, `${path}.${k}`, errs);
      }
    }
    if (t === 'array') {
      if (s.minItems !== undefined && v.length < s.minItems) errs.push(`${path}: fewer than ${s.minItems} items`);
      if (s.maxItems !== undefined && v.length > s.maxItems) errs.push(`${path}: more than ${s.maxItems} items`);
      if (s.items) v.forEach((x, i) => check(x, s.items, file, `${path}[${i}]`, errs));
    }
    if (s.anyOf && !s.anyOf.some((sub) => { const e = []; check(v, sub, file, path, e); return !e.length; }))
      errs.push(`${path}: matches no anyOf branch`);
    if (s.oneOf) {
      const n = s.oneOf.filter((sub) => { const e = []; check(v, sub, file, path, e); return !e.length; }).length;
      if (n !== 1) errs.push(`${path}: matches ${n} oneOf branches`);
    }
  }

  /**
   * Validates `value` against `ref` ("move.json" or "vendor.responses.json#/$defs/get_info").
   * Returns a list of errors (empty = valid).
   */
  return function validate(ref, value, path = '$') {
    const [file, ptr] = ref.split('#');
    const [node, f] = ptr ? resolveRef(ref, file) : [schemas[file], file];
    if (!node) throw new Error(`unknown schema ${ref}`);
    const errs = [];
    check(value, node, f, path, errs);
    return errs;
  };
}
