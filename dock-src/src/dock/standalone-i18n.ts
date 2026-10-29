// Copyright (C) 2026 Parallaxa Labs
// SPDX-License-Identifier: GPL-2.0-or-later
// Translator of the standalone dock: the strings its components use, in all
// three languages, inlined at build time (a file:// page cannot fetch them;
// standalone-dict.gen.ts, made by gen-dict.ts). It does not use $lib/i18n,
// whose loader would inline every group of the app, nor its general ICU-lite
// formatter: `fmt` below covers the shapes these strings use (logic.test.ts
// checks it against format.ts on every standalone string).
import type { Params } from '../lib/i18n/format.ts';
import type { Lang } from './host.ts';
import { EN, ID, JA, KEYS } from './standalone-dict.gen.ts';
import type { Tr } from './t.ts';

// The standalone build passes most keys as their 1-based index in KEYS (scripts/vite-plugins.ts
// dictIndexPlugin); keys built at run time, and every key in tests, stay strings.
const keys = KEYS.split('|');
// EN, ID, JA: the two Latin-script lists next to each other gzip 40 bytes smaller.
const DICTS: Record<Lang, string[]> = { en: EN.split('|'), id: ID.split('|'), ja: JA.split('|') };
const LOCALES: Record<Lang, string> = { en: 'en', ja: 'ja-JP', id: 'id-ID' };

/** `{x}` and `{n, plural, =0 {..} one {..} other {..}}` (`#` = n); a missing param stays as written. */
export function fmt(msg: string, p: Params | undefined, locale: string): string {
  return msg.replace(/\{(\w+)(?:, plural,((?:[^{}]|\{[^{}]*\})*))?\}/g, (all, k: string, arms?: string) => {
    const v = p?.[k];
    if (v === undefined) return all;
    if (arms === undefined) return String(v);
    const n = Number(v);
    const arm: Record<string, string> = {};
    for (const a of arms.matchAll(/(=?\w+)\s*\{([^{}]*)\}/g)) arm[a[1] as string] = a[2] as string;
    return (arm['=' + n] ?? arm[new Intl.PluralRules(locale).select(n)] ?? arm.other ?? all).replace(/#/g, String(n));
  });
}

/** A translator that follows `lang()` (rune state, so templates re-render on a switch). */
export function translator(lang: () => Lang): Tr {
  return (key, params) => {
    const l = lang();
    const k = key as string | number;
    const msg = DICTS[l][typeof k === 'number' ? k - 1 : keys.indexOf(k)];
    return msg === undefined ? key : fmt(msg, params, LOCALES[l]);
  };
}
