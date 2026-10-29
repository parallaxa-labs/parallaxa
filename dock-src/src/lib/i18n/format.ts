// Copyright (C) 2026 Parallaxa Labs
// SPDX-License-Identifier: GPL-2.0-or-later
// ICU-lite message formatting: `{name}` interpolation and
// `{count, plural, =0 {...} one {...} other {...}}` with `#` = the number.
// About 1 KB minified; no i18n framework.

export type Params = Record<string, string | number>;

const rules = new Map<string, Intl.PluralRules>();
const numbers = new Map<string, Intl.NumberFormat>();

function plural(locale: string): Intl.PluralRules {
  let r = rules.get(locale);
  if (!r) rules.set(locale, (r = new Intl.PluralRules(locale)));
  return r;
}

export function formatNumber(n: number, locale: string): string {
  let f = numbers.get(locale);
  if (!f) numbers.set(locale, (f = new Intl.NumberFormat(locale)));
  return f.format(n);
}

/** Index of the brace closing the one at `open`, or -1. */
function closing(s: string, open: number): number {
  let depth = 0;
  for (let i = open; i < s.length; i++) {
    if (s[i] === '{') depth++;
    else if (s[i] === '}' && --depth === 0) return i;
  }
  return -1;
}

function value(v: string | number | undefined, name: string, locale: string): string {
  if (v === undefined) return `{${name}}`;
  return typeof v === 'number' ? formatNumber(v, locale) : v;
}

/** Picks the plural arm for n from `=0 {..} one {..} other {..}`. */
function pickArm(arms: string, n: number, locale: string): string | undefined {
  const found = new Map<string, string>();
  let i = 0;
  while (i < arms.length) {
    const open = arms.indexOf('{', i);
    if (open < 0) break;
    const sel = arms.slice(i, open).trim();
    const end = closing(arms, open);
    if (end < 0) break;
    found.set(sel, arms.slice(open + 1, end));
    i = end + 1;
  }
  return found.get(`=${n}`) ?? found.get(plural(locale).select(n)) ?? found.get('other');
}

export function format(msg: string, params: Params | undefined, locale: string, hash?: string): string {
  if (!msg.includes('{') && !(hash !== undefined && msg.includes('#'))) return msg;
  let out = '';
  let i = 0;
  while (i < msg.length) {
    const ch = msg[i] as string;
    if (ch === '#' && hash !== undefined) {
      out += hash;
      i++;
      continue;
    }
    if (ch !== '{') {
      out += ch;
      i++;
      continue;
    }
    const end = closing(msg, i);
    if (end < 0) {
      out += msg.slice(i);
      break;
    }
    const inner = msg.slice(i + 1, end);
    const m = /^\s*([a-zA-Z_][a-zA-Z0-9_]*)\s*(?:,\s*plural\s*,([\s\S]*))?$/.exec(inner);
    if (!m) out += msg.slice(i, end + 1);
    else if (m[2] === undefined) out += value(params?.[m[1] as string], m[1] as string, locale);
    else {
      const raw = params?.[m[1] as string];
      const n = typeof raw === 'number' ? raw : Number(raw);
      const arm = Number.isFinite(n) ? pickArm(m[2], n, locale) : undefined;
      out += arm === undefined ? msg.slice(i, end + 1) : format(arm, params, locale, formatNumber(n, locale));
    }
    i = end + 1;
  }
  return out;
}
