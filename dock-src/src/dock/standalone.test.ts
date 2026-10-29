// The real standalone Stage dock build (`vite build --mode standalone`): one
// self-contained HTML file for a file:// OBS Custom Browser Dock, with every
// script, style and string inline, the strings in EN, JA and ID, no Studio
// client code, and an up-to-date generated dictionary.
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { gzipSync } from 'node:zlib';
import { build } from 'vite';
import { afterAll, describe, expect, it } from 'vitest';
import { generate, OUT } from './gen-dict.ts';
import { KEYS } from './standalone-dict.gen.ts';

const root = join(dirname(fileURLToPath(import.meta.url)), '../..');
const out = mkdtempSync(join(tmpdir(), 'ails-stage-dock-'));
afterAll(() => rmSync(out, { recursive: true, force: true }));
const kb = (n: number) => Math.round((n / 1024) * 10) / 10;

/**
 * The standalone plugin dock cap (Parallaxa Labs size budget, ratified 2026):
 * at most 32 KB gzipped JS (every inline module script, strings in three languages
 * included).
 */
export const STANDALONE_JS_KB = 32;

describe('standalone Stage dock (file://)', () => {
  it('keeps the generated dictionary in sync with the i18n sources', () => {
    expect(readFileSync(OUT, 'utf8')).toBe(generate());
  });

  it('keeps only the strings the standalone dock uses in the i18n sources', () => {
    // These files are published under the GPL: a sync from AI Live Studio's
    // copy must not bring its other UI strings along.
    const used = new Set(KEYS.split('|'));
    for (const dir of ['src/lib/i18n', 'src/dock/i18n']) {
      for (const l of ['en', 'ja', 'id']) {
        const d = JSON.parse(readFileSync(join(root, dir, `${l}.json`), 'utf8')) as Record<string, string>;
        expect(Object.keys(d).filter((k) => !used.has(k)), `${dir}/${l}.json`).toEqual([]);
      }
    }
  });

  it('builds one self-contained page', { timeout: 60_000 }, async () => {
    // A production build like `pnpm build:standalone` (vitest sets NODE_ENV=test, which
    // would pull in Svelte's dev runtime).
    const env = process.env.NODE_ENV;
    process.env.PARALLAXA_DOCK_OUT = out;
    process.env.NODE_ENV = 'production';
    try {
      await build({ root, configFile: join(root, 'vite.config.ts'), mode: 'standalone', logLevel: 'error' });
    } finally {
      delete process.env.PARALLAXA_DOCK_OUT;
      process.env.NODE_ENV = env;
    }
    const html = readFileSync(join(out, 'dock-standalone.html'), 'utf8');
    expect(html).not.toMatch(/<script[^>]+src=/);
    expect(html).not.toMatch(/<link[^>]+rel="(stylesheet|modulepreload)"/);
    expect(html).toMatch(/<style>@layer reset, tokens, base, components, screens, utilities;<\/style>/);
    // All three languages inline; nothing fetched; no Studio client or loader.
    expect(html).toContain('緊急リセット'); // stage.panic (ja)
    expect(html).toContain('Reset darurat'); // stage.panic (id)
    expect(html).toContain('Panic reset');
    expect(html).not.toMatch(/fetch\(/);
    expect(html).not.toContain('/ws');
    expect(html).not.toContain('stage.call');
    expect(html).toContain('parallaxa'); // vendor name

    const js = [...html.matchAll(/<script type="module">([\s\S]*?)<\/script>/g)].map((m) => m[1]).join('');
    const size = { js: kb(gzipSync(js, { level: 9 }).length), total: kb(gzipSync(html, { level: 9 }).length) };
    console.log(`dock-standalone.html: ${size.js} KB JS, ${size.total} KB in all (gzip -9)`);
    expect(size.js, `standalone dock JS ${size.js} KB > ${STANDALONE_JS_KB} KB (size budget)`).toBeLessThanOrEqual(STANDALONE_JS_KB);
  });
});
