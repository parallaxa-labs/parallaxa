// Copyright (C) 2026 Parallaxa Labs
// SPDX-License-Identifier: GPL-2.0-or-later
// Hold-to-confirm: `run` fires after the pointer, Enter or Space
// has been held for HOLD_MS; releasing early cancels. Returns event handlers
// for a button (plain handlers, no Svelte action runtime). The fill sweep is
// CSS (`.holding`); the timer works without it (reduced motion).
export const HOLD_MS = 600;

export function holdable(run: () => void) {
  let timer: ReturnType<typeof setTimeout> | undefined;
  let el: HTMLElement | null = null;
  const stop = () => {
    clearTimeout(timer);
    timer = undefined;
    el?.classList.remove('holding');
  };
  const start = (e: Event) => {
    if (e instanceof KeyboardEvent && (e.repeat || (e.key !== 'Enter' && e.key !== ' '))) return;
    e.preventDefault(); // Enter would also click: a click is never a confirmation
    if (timer) return;
    el = e.currentTarget as HTMLElement;
    el.classList.add('holding');
    timer = setTimeout(() => {
      stop();
      run();
    }, HOLD_MS);
  };
  return { start, stop };
}
