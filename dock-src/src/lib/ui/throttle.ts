// Copyright (C) 2026 Parallaxa Labs
// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * Calls fn at most once per `ms` (leading + trailing): the last value of a
 * burst is always delivered. Used by sliders (30 Hz live updates).
 */
export function throttle<A extends unknown[]>(fn: (...args: A) => void, ms: number): ((...args: A) => void) & { flush(): void; cancel(): void } {
  let last = 0;
  let timer: ReturnType<typeof setTimeout> | null = null;
  let pending: A | null = null;
  const run = () => {
    timer = null;
    last = Date.now();
    const args = pending;
    pending = null;
    if (args) fn(...args);
  };
  const call = (...args: A) => {
    pending = args;
    const wait = ms - (Date.now() - last);
    if (wait <= 0 && !timer) run();
    else timer ??= setTimeout(run, Math.max(0, wait));
  };
  call.flush = () => {
    if (timer) clearTimeout(timer);
    if (pending) run();
    else timer = null;
  };
  call.cancel = () => {
    if (timer) clearTimeout(timer);
    timer = null;
    pending = null;
  };
  return call;
}

/** Decimal places implied by a step (0.05 -> 2). */
export function decimals(step: number): number {
  const s = String(step);
  const i = s.indexOf('.');
  return i < 0 ? 0 : s.length - i - 1;
}

/** Clamps v to [min, max] and snaps it to the step grid from min. */
export function snap(v: number, min: number, max: number, step: number): number {
  if (!Number.isFinite(v)) return min;
  const n = Math.round((v - min) / step) * step + min;
  return Number(Math.min(max, Math.max(min, n)).toFixed(decimals(step)));
}
