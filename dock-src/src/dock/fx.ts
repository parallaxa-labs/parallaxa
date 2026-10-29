// Copyright (C) 2026 Parallaxa Labs
// SPDX-License-Identifier: GPL-2.0-or-later
// Stage FX data the remote UI can know (Effects card and its sliders).
// Protocol v1 cannot list FX presets or report FX parameter values, so the
// shipped presets (Parallaxa data/presets/fx) are static data here; logic.test.ts
// checks this copy against the preset files.
import type { FxParams } from '../lib/protocol/vendor.ts';

export type FxNum = Exclude<keyof FxParams, 'lut' | 'leak'>;

/** One effect slider: protocol range and neutral; the UI shows value × scale (as the OBS panel, stage-plugin-ux.md 2.1). */
export interface FxSlider {
  p: FxNum;
  /** Label group key suffix: stage.fx_group.colour or stage.fx.<g>. */
  g: 'colour' | 'lut' | 'vignette' | 'bloom' | 'chroma' | 'grain' | 'leak';
  min: number;
  max: number;
  neutral: number;
  scale: number;
  /** UI step. */
  step: number;
  unit: string;
  /** Shown without opening "More effects" (the package spec's six). */
  main?: 1;
}

const pct = (p: FxNum, g: FxSlider['g'], min: number, max: number, neutral: number, main?: 1): FxSlider => ({
  p,
  g,
  min,
  max,
  neutral,
  scale: 100,
  step: 1,
  unit: '%',
  ...(main ? { main } : {}),
});

export const FX_SLIDERS: readonly FxSlider[] = [
  pct('vignette', 'vignette', 0, 1, 0, 1),
  pct('grain', 'grain', 0, 1, 0, 1),
  pct('chroma', 'chroma', 0, 1, 0, 1),
  pct('saturation', 'colour', -1, 1, 0, 1),
  pct('contrast', 'colour', -1, 1, 0, 1),
  pct('lut_amount', 'lut', 0, 1, 0, 1),
  { p: 'exposure', g: 'colour', min: -2, max: 2, neutral: 0, scale: 1, step: 0.05, unit: 'EV' },
  pct('temperature', 'colour', -1, 1, 0),
  pct('tint', 'colour', -1, 1, 0),
  pct('vignette_soft', 'vignette', 0, 1, 0.5),
  pct('vignette_round', 'vignette', 0, 1, 1),
  pct('bloom', 'bloom', 0, 1, 0),
  pct('bloom_threshold', 'bloom', 0, 1, 0.8),
  { p: 'grain_size', g: 'grain', min: 0.5, max: 4, neutral: 1, scale: 1, step: 0.1, unit: 'x' },
  pct('leak_amount', 'leak', 0, 1, 0),
  pct('leak_speed', 'leak', 0, 2, 0.2),
];

/** Built-in FX presets (Parallaxa data/presets/fx/<id>.json `params`; absent = neutral). */
export const FX_PRESETS: Readonly<Record<string, FxParams>> = {
  neutral: {},
  cozy: { exposure: 0.1, contrast: 0.08, saturation: 0.05, temperature: 0.25, vignette: 0.25, vignette_soft: 0.6, grain: 0.08, bloom: 0.3, bloom_threshold: 0.75 },
  crisp: { contrast: 0.1, saturation: 0.1, vignette: 0.12, vignette_soft: 0.7, bloom: 0.15, bloom_threshold: 0.85 },
  dreamy: { exposure: 0.15, contrast: -0.1, saturation: -0.05, tint: 0.08, vignette: 0.2, vignette_soft: 0.8, chroma: 0.08, bloom: 0.55, bloom_threshold: 0.6 },
  film: { contrast: 0.12, saturation: -0.08, temperature: 0.08, vignette: 0.3, vignette_soft: 0.7, chroma: 0.05, grain: 0.18, grain_size: 1.4, lut: 'film', lut_amount: 0.7 },
  horror: { exposure: -0.35, contrast: 0.3, saturation: -0.55, temperature: -0.35, tint: -0.1, vignette: 0.6, vignette_soft: 0.45, vignette_round: 0.8, chroma: 0.15, grain: 0.3, lut: 'night', lut_amount: 0.6 },
  hype: { exposure: 0.05, contrast: 0.2, saturation: 0.35, vignette: 0.15, chroma: 0.12, bloom: 0.4, bloom_threshold: 0.7 },
  lofi: { exposure: -0.05, contrast: -0.08, saturation: -0.12, temperature: 0.12, tint: 0.05, vignette: 0.4, vignette_soft: 0.75, grain: 0.22, grain_size: 1.8, lut: 'film', lut_amount: 0.5, bloom: 0.3, bloom_threshold: 0.65 },
  rainy: { exposure: -0.1, contrast: -0.05, saturation: -0.25, temperature: -0.2, vignette: 0.35, vignette_soft: 0.7, grain: 0.2, grain_size: 1.6, lut: 'night', lut_amount: 0.35, bloom: 0.25, bloom_threshold: 0.7 },
  spotlight: { exposure: 0.1, contrast: 0.15, vignette: 0.55, vignette_soft: 0.5, vignette_round: 1, bloom: 0.2, bloom_threshold: 0.8 },
};

/** Shipped LUT ids offered remotely (the test LUT `identity` is not offered); '' = None. */
export const LUTS = ['', 'film', 'night'] as const;

/**
 * The value a slider shows: 1. sent from this page, 2. the
 * built-in preset's value (absent = neutral), 3. undefined = unknown.
 */
export function fxValue(p: keyof FxParams, sent: FxParams, preset: string): number | string | undefined {
  if (sent[p] !== undefined) return sent[p];
  const ps = FX_PRESETS[preset];
  if (!ps) return undefined;
  if (ps[p] !== undefined) return ps[p];
  return p === 'lut' || p === 'leak' ? '' : FX_SLIDERS.find((s) => s.p === p)?.neutral;
}
