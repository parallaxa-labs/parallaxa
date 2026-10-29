// Copyright (C) 2026 Parallaxa Labs
// SPDX-License-Identifier: GPL-2.0-or-later
// The Stage UI translates through this indirection so the same components run
// with the shared rune i18n of the web UI and in the standalone file:// dock
// (its own inlined dictionaries, standalone-i18n.ts) without the standalone
// bundle pulling in the web UI's dictionary loader. Both translators read rune
// state, so templates still re-render when the language changes.
import type { Params } from '../lib/i18n/format.ts';

export type Tr = (key: string, params?: Params) => string;

let impl: Tr = (k) => k;

export const t: Tr = (key, params) => impl(key, params);

/** Installs the page's translator (once, before mounting). */
export function setTranslator(fn: Tr): void {
  impl = fn;
}
