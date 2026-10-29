// Copyright (C) 2026 Parallaxa Labs
// SPDX-License-Identifier: GPL-2.0-or-later
/** True when typing in this element should not trigger single-key shortcuts. */
export function isTextTarget(el: EventTarget | null): boolean {
  const e = el as (HTMLElement & { type?: string }) | null;
  if (!e || typeof e.tagName !== 'string') return false;
  if (e.isContentEditable) return true;
  const tag = e.tagName;
  if (tag === 'TEXTAREA' || tag === 'SELECT') return true;
  if (tag === 'INPUT') return !['button', 'checkbox', 'radio', 'range', 'submit', 'reset', 'color', 'file'].includes(e.type ?? 'text');
  return false;
}
