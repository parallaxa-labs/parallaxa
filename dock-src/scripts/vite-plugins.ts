// Copyright (C) 2026 Parallaxa Labs
// SPDX-License-Identifier: GPL-2.0-or-later
// Vite build plugins shared by the web UI and the standalone Stage dock:
// split i18n dictionaries (i18nPlugin) and the self-contained file:// page
// (inlinePlugin). No dependencies beyond Node and Vite.
import { globSync, readFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { dirname, join, resolve } from 'node:path';
import type { Plugin } from 'vite';
import { OUT as DICT_FILE, SOURCES, usedKeys } from '../src/dock/gen-dict.ts';
import { LANGS, groupOf } from '../src/lib/i18n/groups.ts';

type Dict = Record<string, string>;

// ---------------------------------------------------------------------------
// i18n: virtual:i18n/source (+ virtual:i18n/<lang>/<group> outside `vite build`)
// ---------------------------------------------------------------------------

/**
 * Splits the dictionaries into one small file per (language, group).
 * Sources: the shared src/lib/i18n/<lang>.json plus any screen-owned
 * src/{app,dock,overlay}/**\/i18n/<lang>.json, so a screen can add keys
 * without editing the shared files. A screen key may not redefine a shared
 * key. A key missing in ja or id gets the English text (applied at build time
 * so it costs nothing at run time) and a build warning.
 *
 * `virtual:i18n/source` exports `load(group, lang)`:
 * - `vite build` with `json` (pages served over HTTP): the strings are plain
 *   JSON assets, assets/i18n-<lang>-<group>-<hash>.json, fetched on demand. JSON
 *   is data: no loader table, no module wrappers, and it parses faster than JS.
 * - dev server, vitest and the standalone file:// build: lazy `import()` of
 *   virtual:i18n/<lang>/<group> modules (inlined by the standalone build,
 *   where fetch() cannot read file:// URLs).
 */
export function i18nPlugin(root: string, opts: { only?: readonly string[]; json?: boolean } = {}): Plugin {
  const PREFIX = 'virtual:i18n/';
  let dicts: Record<string, Dict> | null = null;
  let build = false;
  const files = new Set<string>();

  function load(warn: (msg: string) => void): Record<string, Dict> {
    if (dicts) return dicts;
    const out = readDicts(root, files);
    fillFromEnglish(out, warn);
    dicts = out;
    return out;
  }

  function groups(warn: (msg: string) => void): string[] {
    const g = new Set<string>();
    for (const d of Object.values(load(warn))) for (const k of Object.keys(d)) g.add(groupOf(k));
    // `only` (standalone builds) keeps just the listed groups: with code
    // splitting off, every loader is inlined into the one page.
    return [...g].filter((x) => !opts.only || opts.only.includes(x)).sort();
  }

  /** One hash for the whole string set: cache-busts every file when any string changes. */
  function hash(warn: (msg: string) => void): string {
    return createHash('sha256').update(JSON.stringify(load(warn))).digest('base64url').slice(0, 8);
  }

  return {
    name: 'ails-i18n',
    configResolved(c) {
      build = c.command === 'build';
    },
    resolveId(id) {
      if (id.startsWith(PREFIX)) return '\0' + id;
      return null;
    },
    load(id) {
      if (!id.startsWith('\0' + PREFIX)) return null;
      const warn = (m: string) => this.warn(m);
      const d = load(warn);
      for (const f of files) this.addWatchFile(f);
      const spec = id.slice(1 + PREFIX.length);
      if (spec === 'source') {
        if (build && opts.json) {
          // Absolute URL: pages in subfolders fetch the same assets.
          return `const H=${JSON.stringify(hash(warn))};export function load(g,l){return fetch("/assets/i18n-"+l+"-"+g+"-"+H+".json").then(r=>r.ok?r.json():{})}`;
        }
        const body = groups(warn)
          .map((g) => `${JSON.stringify(g)}:{${LANGS.map((l) => `${l}:()=>import("${PREFIX}${l}/${g}")`).join(',')}}`)
          .join(',');
        return `const L={${body}};export function load(g,l){const f=L[g]&&L[g][l];return f?f().then(m=>m.default):Promise.resolve({})}`;
      }
      const [lang, group] = spec.split('/');
      const src = d[lang as string];
      if (!src || !group) throw new Error(`i18n: bad module ${spec}`);
      return `export default ${JSON.stringify(slice(src, group))};`;
    },
    generateBundle() {
      if (!opts.json) return;
      const warn = (m: string) => this.warn(m);
      const d = load(warn);
      const h = hash(warn);
      for (const g of groups(warn)) {
        for (const l of LANGS) {
          this.emitFile({ type: 'asset', fileName: `assets/i18n-${l}-${g}-${h}.json`, source: JSON.stringify(slice(d[l] as Dict, g)) });
        }
      }
    },
    handleHotUpdate(ctx) {
      if (files.has(resolve(ctx.file))) {
        dicts = null;
        ctx.server.ws.send({ type: 'full-reload' });
        return [];
      }
      return undefined;
    },
  };
}

/**
 * The full string set per language: src/lib/i18n/<lang>.json plus every
 * screen-owned src/{app,dock,overlay}/**\/i18n/<lang>.json (sorted, so the merge
 * order is stable). A screen key may not redefine a shared key with another
 * text. `files` collects the paths read (for the dev server's watcher).
 * Exported for tests (the "every literal key exists" check).
 */
export function readDicts(root: string, files?: Set<string>): Record<string, Dict> {
  const out: Record<string, Dict> = {};
  for (const lang of LANGS) {
    const base = join(root, 'src/lib/i18n', `${lang}.json`);
    files?.add(base);
    const d: Dict = JSON.parse(readFileSync(base, 'utf8'));
    const extra = globSync(`src/{app,dock,overlay}/**/i18n/${lang}.json`, { cwd: root }).sort();
    for (const rel of extra) {
      const p = join(root, rel);
      files?.add(p);
      const more: Dict = JSON.parse(readFileSync(p, 'utf8'));
      for (const [k, v] of Object.entries(more)) {
        if (k in d && d[k] !== v) throw new Error(`i18n: ${rel} redefines shared key ${k}`);
        d[k] = v;
      }
    }
    out[lang] = d;
  }
  return out;
}

/** The keys of `group` in `d`. */
function slice(d: Dict, group: string): Dict {
  const sub: Dict = {};
  for (const [k, v] of Object.entries(d)) if (groupOf(k) === group) sub[k] = v;
  return sub;
}

/**
 * A key missing in a translation falls back to English. Done once at build
 * time; warns about missing and orphan keys so screen authors see them.
 * Exported for tests.
 */
export function fillFromEnglish(dicts: Record<string, Dict>, warn: (msg: string) => void): void {
  const en = dicts.en ?? {};
  for (const lang of LANGS) {
    const d = dicts[lang];
    if (lang === 'en' || !d) continue;
    const missing = Object.keys(en).filter((k) => !(k in d));
    for (const k of missing) d[k] = en[k] as string;
    if (missing.length) warn(`i18n: ${missing.length} key(s) missing in ${lang}, English used: ${missing.slice(0, 5).join(', ')}`);
    const orphan = Object.keys(d).filter((k) => !(k in en));
    if (orphan.length) warn(`i18n: ${orphan.length} key(s) in ${lang} but not in en: ${orphan.slice(0, 5).join(', ')}`);
  }
}

// ---------------------------------------------------------------------------
// Standalone (file://) pages: inline every script and stylesheet
// ---------------------------------------------------------------------------

/**
 * For the standalone Stage dock opened as a file URL in OBS: Chromium refuses
 * module scripts and stylesheets from file:// across the null origin, so the
 * HTML must carry everything inline. Used only by `vite build --mode standalone`
 * with code splitting off (one JS chunk, one CSS asset).
 *
 * It also keeps the standalone dock under its 32 KB gz JS cap with room to
 * grow, in every build that inlines it: shallow rune state only
 * (rawStatePlugin, about 0.75 KB), numbered dictionary keys (dictIndexPlugin,
 * about 0.85 KB) and a third terser pass (about 0.1 KB).
 */
export function inlinePlugin(rename: Record<string, string>): Plugin[] {
  const passes: Plugin = { name: 'ails-standalone-terser', config: () => ({ build: { terserOptions: { compress: { passes: 3 } } } }) };
  return [rawStatePlugin(), dictIndexPlugin(), passes, inline(rename)];
}

const slash = (p: string) => p.replace(/\\/g, '/');

/** Stands in for svelte/src/internal/client/proxy.js in standalone builds (rawStatePlugin). */
export const PROXY_STUB =
  'export function proxy(){throw new Error("standalone page: deep $state is left out of this build, use $state.raw")}' +
  'export function get_proxied_value(v){return v}export const is=Object.is;';

/**
 * Standalone pages use shallow rune state only ($state.raw; no deep $state, no
 * $bindable props), so Svelte's deep-state proxy is left out: its module becomes
 * PROXY_STUB, whose proxy() throws. The runtime only calls proxy() for code that
 * declares a deep $state or binds a $bindable prop, and a build whose own
 * sources do either fails here. Exported for tests.
 */
export function rawStatePlugin(): Plugin {
  return {
    name: 'ails-raw-state',
    apply: 'build',
    enforce: 'pre', // the sources as written, before the Svelte compiler
    load(id) {
      return slash(id).endsWith('/svelte/src/internal/client/proxy.js') ? PROXY_STUB : null;
    },
    transform(code, id) {
      if (id.startsWith('\0') || slash(id).includes('/node_modules/') || !/\.(svelte|ts|js)$/.test(id.split('?')[0] as string)) return null;
      const m = /\$state\s*[(<]|\$bindable\s*\(/.exec(code);
      if (m) this.error(`raw-state: ${m[0]} in ${id}: a standalone page keeps rune state shallow, use $state.raw (see scripts/vite-plugins.ts)`);
      return null;
    },
  };
}

/**
 * The standalone dock's strings without their keys (for the 32 KB gz JS cap):
 * in the dock sources (gen-dict SOURCES) every literal key of
 * standalone-dict.gen.ts becomes its 1-based index (never 0, so `key ? t(key)
 * : ..` still holds), which standalone-i18n.ts reads straight from the value
 * lists. Keys built at run time (`stage.fx.${id}`) keep a string: their template
 * prefix becomes one capital letter, here and in the generated KEYS, and every
 * other entry of KEYS is emptied. The committed files keep the full keys (the
 * string catalogues hold only the keys KEYS lists). Exported for tests.
 */
export function dictIndexPlugin(): Plugin {
  const sources = new Set(SOURCES.map((f) => slash(join(dirname(DICT_FILE), f))));
  let packed = '';
  const index = new Map<string, number>();
  const short = new Map<string, string>();
  return {
    name: 'ails-dict-index',
    apply: 'build',
    enforce: 'post', // after the Svelte compiler and the TypeScript transform
    buildStart() {
      const m = /export const KEYS = "([^"]*)"/.exec(readFileSync(DICT_FILE, 'utf8'));
      if (!m) throw new Error(`dict-index: no KEYS in ${DICT_FILE}`);
      const keys = (m[1] as string).split('|');
      const prefixes = [...usedKeys()[1]].sort((a, b) => b.length - a.length); // longest first
      if (prefixes.length > 26) throw new Error('dict-index: more than 26 key prefixes');
      short.clear();
      prefixes.forEach((p, i) => short.set(p, String.fromCharCode(65 + i)));
      index.clear();
      keys.forEach((k, i) => index.set(k, i + 1));
      packed = keys
        .map((k) => {
          const p = prefixes.find((x) => k.startsWith(x));
          return p ? short.get(p) + k.slice(p.length) : '';
        })
        .join('|');
    },
    transform(code, id) {
      const file = slash(id);
      if (file === slash(DICT_FILE)) {
        const out = code.replace(/(KEYS = )"[^"]*"/, (_, a: string) => a + JSON.stringify(packed));
        if (out === code) throw new Error('dict-index: KEYS not rewritten');
        return { code: out, map: null };
      }
      if (!sources.has(file)) return null;
      // 'stage.x' / "stage.x" / `stage.x` -> its index; `stage.fx.${ -> `A${.
      const out = code.replace(/(['"`])((?:stage|stage_sa|recipe|ui)\.[a-z0-9_.]*)(\$\{|['"`])/g, (all, q: string, k: string, end: string) => {
        if (end === '${') return q === '`' && short.has(k) ? q + short.get(k) + end : all;
        return end === q && index.has(k) ? String(index.get(k)) : all;
      });
      return out === code ? null : { code: out, map: null };
    },
  };
}

function inline(rename: Record<string, string>): Plugin {
  return {
    name: 'ails-inline',
    apply: 'build',
    enforce: 'post',
    generateBundle(_opts, bundle) {
      for (const html of Object.values(bundle)) {
        if (html.type !== 'asset' || !html.fileName.endsWith('.html')) continue;
        let src = String(html.source);
        for (const [name, item] of Object.entries(bundle)) {
          const ref = new RegExp(`<script[^>]*src="[./]*${escape(name)}"[^>]*></script>`);
          const css = new RegExp(`<link[^>]*href="[./]*${escape(name)}"[^>]*>`);
          if (item.type === 'chunk' && ref.test(src)) {
            src = src.replace(ref, () => `<script type="module">${item.code.replace(/<\/script/gi, '<\\/script')}</script>`);
            delete bundle[name];
          } else if (item.type === 'asset' && name.endsWith('.css') && css.test(src)) {
            src = src.replace(css, () => `<style>${String(item.source)}</style>`);
            delete bundle[name];
          }
        }
        src = src.replace(/<link rel="modulepreload"[^>]*>/g, '');
        html.source = src;
        const to = rename[html.fileName];
        if (to) html.fileName = to;
      }
    },
  };
}

function escape(s: string): string {
  return s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}
