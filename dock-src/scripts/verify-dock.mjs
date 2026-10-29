#!/usr/bin/env node
// Proves that this folder rebuilds the shipped standalone dock: runs the
// production build into a temporary folder and compares the result with
// ../data/dock/dock-standalone.html (line endings normalised). A difference
// means the shipped file is stale (run `pnpm build`) or a source is missing.
//
//   node scripts/verify-dock.mjs        (part of `pnpm verify`)
//
// Copyright (C) 2026 Parallaxa Labs
// SPDX-License-Identifier: GPL-2.0-or-later
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { build } from 'vite';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const shipped = join(root, '..', 'data', 'dock', 'dock-standalone.html');
const out = mkdtempSync(join(tmpdir(), 'parallaxa-dock-'));
const norm = (s) => s.replace(/\r\n/g, '\n');
try {
  process.env.PARALLAXA_DOCK_OUT = out;
  process.env.NODE_ENV = 'production';
  await build({ root, configFile: join(root, 'vite.config.ts'), mode: 'standalone', logLevel: 'error' });
  const a = norm(readFileSync(join(out, 'dock-standalone.html'), 'utf8'));
  const b = norm(readFileSync(shipped, 'utf8'));
  if (a !== b) {
    console.error(`verify-dock: dock-src/ does not rebuild data/dock/dock-standalone.html (${a.length} vs ${b.length} chars). Run \`pnpm build\` and commit the result.`);
    process.exitCode = 1;
  } else {
    console.log(`verify-dock: dock-src/ rebuilds data/dock/dock-standalone.html identically (${b.length} chars)`);
  }
} finally {
  rmSync(out, { recursive: true, force: true });
}
