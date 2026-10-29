// Build of the standalone Stage dock: one self-contained HTML file
// (data/dock/dock-standalone.html) that OBS opens as a file:// Custom Browser
// Dock. Every script, style and string is inlined (file:// pages cannot load
// module scripts), the strings in EN, JA and ID.
//
//   pnpm build    vite build --mode standalone -> ../data/dock/dock-standalone.html
//   PARALLAXA_DOCK_OUT=<dir> overrides the output folder (tests, verify-dock.mjs).
//
// The build options match the AI Live Studio web build's standalone mode, so
// both produce the same file.
//
// Copyright (C) 2026 Parallaxa Labs
// SPDX-License-Identifier: GPL-2.0-or-later
import { svelte } from '@sveltejs/vite-plugin-svelte';
import { fileURLToPath } from 'node:url';
import { resolve } from 'node:path';
import { defaultClientConditions } from 'vite';
import { defineConfig } from 'vitest/config';
import { i18nPlugin, inlinePlugin } from './scripts/vite-plugins.ts';

const root = fileURLToPath(new URL('.', import.meta.url));
const r = (p: string) => resolve(root, p);
const entry = 'src/dock/standalone.html';

export default defineConfig({
  root,
  base: './',
  publicDir: false,
  resolve: {
    alias: { $lib: r('src/lib') },
    // Component tests mount Svelte in jsdom: use the client build of svelte.
    ...(process.env.VITEST ? { conditions: [...defaultClientConditions, 'browser'] } : {}),
  },
  plugins: [svelte(), i18nPlugin(root, { only: ['core', 'stage', 'recipe'] }), inlinePlugin({ [entry]: 'dock-standalone.html' })],
  build: {
    outDir: r(process.env.PARALLAXA_DOCK_OUT ?? '../data/dock'),
    emptyOutDir: false,
    target: 'chrome120', // OBS 32 = CEF 127, OBS 33 = CEF 150
    manifest: false,
    modulePreload: { polyfill: false },
    assetsInlineLimit: Number.MAX_SAFE_INTEGER,
    cssCodeSplit: false,
    reportCompressedSize: false,
    minify: 'terser',
    terserOptions: { ecma: 2020, module: true, compress: { passes: 2, pure_getters: true }, format: { comments: false } },
    rollupOptions: {
      input: { standalone: r(entry) },
      output: { inlineDynamicImports: true },
    },
  },
  test: {
    include: ['src/**/*.test.ts'],
    environment: 'node',
  },
});
