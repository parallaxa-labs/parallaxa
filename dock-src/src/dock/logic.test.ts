// Pure logic of the Stage dock: SHA-256 / obs-websocket auth, the FX data
// against the shipped presets, effect value rules, the role guess and the
// standalone dock's string formatter.
import { createHash } from 'node:crypto';
import { readdirSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';
import { FX_PRESETS, FX_SLIDERS, fxValue, LUTS } from './fx.ts';
import { guessRole } from './model.svelte.ts';
import { format, type Params } from '../lib/i18n/format.ts';
import { obsAuth, sha256, sha256b64 } from './sha256.ts';
import { EN, ID, JA } from './standalone-dict.gen.ts';
import { fmt } from './standalone-i18n.ts';

const hex = (b: Uint8Array) => [...b].map((x) => x.toString(16).padStart(2, '0')).join('');
const enc = (s: string) => new TextEncoder().encode(s);

describe('sha256', () => {
  it('matches the FIPS 180-4 test vectors', () => {
    expect(hex(sha256(enc('')))).toBe('e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855');
    expect(hex(sha256(enc('abc')))).toBe('ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
    expect(hex(sha256(enc('abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq')))).toBe(
      '248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1',
    );
  });

  it('matches node:crypto across block boundaries and UTF-8 input', () => {
    for (const n of [1, 55, 56, 63, 64, 65, 119, 120, 1000]) {
      const s = 'x'.repeat(n) + 'パスワード';
      expect(sha256b64(s)).toBe(createHash('sha256').update(s, 'utf8').digest('base64'));
    }
  });

  it('builds the obs-websocket v5 authentication string', () => {
    const [pw, salt, challenge] = ['supersecretpassword', 'lM1GncleQOaCu9lT1yeUZhFYnqhsLLP1G5lAGo3ixaI=', '+IxH4CnCiqpX1rM9scsNynZzbOe4KhDeYcTNS3PDaeY='];
    const b64 = (s: string) => createHash('sha256').update(s).digest('base64');
    expect(obsAuth(pw, salt, challenge)).toBe(b64(b64(pw + salt) + challenge));
  });
});

describe('FX data (vendor-api.md 4.7)', () => {
  const dir = fileURLToPath(new URL('../../../data/presets/fx/', import.meta.url));
  const files = readdirSync(dir).filter((f) => f.endsWith('.json'));

  it('carries exactly the shipped FX presets and their params', () => {
    expect(Object.keys(FX_PRESETS).sort()).toEqual(files.map((f) => f.replace('.json', '')).sort());
    for (const f of files) {
      const p = JSON.parse(readFileSync(dir + f, 'utf8')) as { id: string; params: object };
      expect(FX_PRESETS[p.id], p.id).toEqual(p.params);
    }
  });

  it('offers the shipped LUTs except the test LUT', () => {
    const luts = readdirSync(fileURLToPath(new URL('../../../data/presets/luts/', import.meta.url)))
      .filter((f) => f.endsWith('.png') && f !== 'identity.png')
      .map((f) => f.replace('.png', ''));
    expect(LUTS.filter(Boolean).sort()).toEqual(luts.sort());
  });

  it('uses the protocol ranges and neutrals (vendor-api.md 4.7)', () => {
    const want: Record<string, [number, number, number]> = {
      exposure: [-2, 2, 0], contrast: [-1, 1, 0], saturation: [-1, 1, 0], temperature: [-1, 1, 0], tint: [-1, 1, 0],
      vignette: [0, 1, 0], vignette_soft: [0, 1, 0.5], vignette_round: [0, 1, 1], chroma: [0, 1, 0], grain: [0, 1, 0],
      grain_size: [0.5, 4, 1], lut_amount: [0, 1, 0], leak_amount: [0, 1, 0], leak_speed: [0, 2, 0.2], bloom: [0, 1, 0], bloom_threshold: [0, 1, 0.8],
    };
    expect(Object.fromEntries(FX_SLIDERS.map((s) => [s.p, [s.min, s.max, s.neutral]]))).toEqual(want);
    // The package spec's six are the visible ones.
    expect(FX_SLIDERS.filter((s) => s.main).map((s) => s.p).sort()).toEqual(['chroma', 'contrast', 'grain', 'lut_amount', 'saturation', 'vignette']);
  });

  it('shows sent values, then the preset value, else unknown', () => {
    expect(fxValue('vignette', { vignette: 0.5 }, 'cozy')).toBe(0.5);
    expect(fxValue('vignette', {}, 'cozy')).toBe(0.25);
    expect(fxValue('bloom_threshold', {}, 'film')).toBe(0.8); // absent = neutral
    expect(fxValue('lut', {}, 'cozy')).toBe('');
    expect(fxValue('vignette', {}, 'my_preset')).toBeUndefined();
    expect(fxValue('vignette', {}, '')).toBeUndefined();
  });
});

describe('role guess (vendor-api.md 4.13)', () => {
  it('matches the plugin rules', () => {
    expect(guessRole('BG')).toBe('bg');
    expect(guessRole('Night Background')).toBe('bg');
    expect(guessRole('背景')).toBe('bg');
    expect(guessRole('FG Desk')).toBe('fg');
    expect(guessRole('前景の机')).toBe('fg');
    expect(guessRole('VTube Studio')).toBe('char');
    expect(guessRole('Avatar Spout')).toBe('char');
    expect(guessRole('vts')).toBe('char');
    expect(guessRole('Chat Box')).toBe('none');
    expect(guessRole('subgroup')).toBe('none'); // "bg" only as a word
  });
});

describe('standalone formatter', () => {
  it('formats every standalone string like the shared ICU-lite formatter', () => {
    const locales = { en: 'en', ja: 'ja-JP', id: 'id-ID' };
    const params: (Params | undefined)[] = [undefined, { count: 0, s: 10, name: 'Cozy', what: 'Zoom', slot: 3, version: '0.1.0', scene: 'Chat', effect: 'Bloom' }, { count: 1 }, { count: 7 }];
    for (const [lang, list] of [['en', EN], ['ja', JA], ['id', ID]] as const) {
      for (const msg of list.split('|')) for (const p of params) expect(fmt(msg, p, locales[lang])).toBe(format(msg, p, locales[lang]));
    }
    expect(fmt('{count, plural, =0 {none} one {# item} other {# items}}', { count: 1 }, 'en')).toBe('1 item');
  });
});
