// Copyright (C) 2026 Parallaxa Labs
// SPDX-License-Identifier: GPL-2.0-or-later
// i18n chunking rule, shared by the Vite plugin (build time) and the runtime.
//
// Every key belongs to the group named by its first segment (`chat.empty` is in
// group `chat`), except the shell prefixes below, which are merged into `core`.
// Each (language, group) pair becomes its own tiny lazy chunk, so a dock or an
// overlay only downloads the strings it shows (size budgets).

export const LANGS = ['en', 'ja', 'id'] as const;
export type Lang = (typeof LANGS)[number];

/** Prefixes bundled into the `core` group, loaded by every app and dock page. */
export const CORE_PREFIXES: readonly string[] = [
  'app',
  'nav',
  'status',
  'health',
  'panic',
  'mode',
  'common',
  'shell',
  'director',
  'ui',
];

/** The group a key belongs to. */
export function groupOf(key: string): string {
  const dot = key.indexOf('.');
  const prefix = dot < 0 ? key : key.slice(0, dot);
  return CORE_PREFIXES.includes(prefix) ? 'core' : prefix;
}
