<!--
  Copyright (C) 2026 Parallaxa Labs
  SPDX-License-Identifier: GPL-2.0-or-later
-->
<!--
  Compact live-safe slider for the Stage controls:
  - `value` undefined = unknown: the field shows "—", the thumb is hollow at
    `def`, and nothing is sent until the user moves it.
  - `oninput` is the only sender: throttled to 30 Hz while dragging, the final value delivered
    on commit, and a value already sent is never sent twice. `onchange` fires once per commit
    that changed a known value, with the value before the interaction: for the undo stack only.
  - `locked` (on-air lock) disables it with an explanation; Shift+Arrow steps x10, Alt+Arrow
    x0.1 (off the step grid, shown with one more decimal). While `onair`, holding Alt is the
    unlock, so Alt+Arrow keeps the normal step there.
  - Double-click the label, or the reset button shown when the value is not `def`, resets it.
  - The unit's leading space is part of an expression: Svelte 5 trims whitespace at the start
    of an element, which read "Amount(%)".
-->
<script lang="ts">
  import { decimals, snap, throttle } from '../lib/ui/throttle.ts';
  import { t } from './t.ts';

  interface Props {
    label: string;
    value: number | undefined;
    min: number;
    max: number;
    step?: number;
    unit?: string;
    def?: number;
    locked?: boolean;
    /** OBS streams: Alt means "unlock", not "fine step". */
    onair?: boolean;
    disabled?: boolean;
    /** Words for both ends ("Far", "Near"), shown under the track. */
    ends?: [string, string];
    oninput?: (v: number) => void;
    onchange?: (prev: number) => void;
  }
  // Defaults by hand: a destructuring default pulls Svelte's prop() helper (0.4 KB gz) into the
  // standalone dock. locked / onair / disabled / unit simply stay falsy when not given.
  let { label, value, min, max, step: stepIn, unit, def: defIn, locked, onair, disabled, ends, oninput, onchange }: Props = $props();
  const step = $derived(stepIn ?? 1);
  const def = $derived(defIn ?? min);

  const uid = $props.id();
  /** The value while (and right after) the user moves the slider. */
  let local = $state.raw<number | undefined>(undefined);
  /** Value when the interaction started; null while idle. */
  let start: number | undefined | null = null;
  /** Last value handed to oninput in this interaction. */
  let sent: number | undefined;
  const live = throttle((v: number) => v !== sent && oninput?.((sent = v)), 33);
  $effect(() => () => live.cancel());
  const off = $derived(locked || disabled);
  // A new value from outside replaces the local one, except mid-drag. A drag cut short
  // without a `change` (the on-air lock engaged mid-drag) ends here too.
  $effect.pre(() => {
    void value;
    if (off) {
      live.cancel();
      start = null;
    }
    if (start === null) local = undefined;
  });

  const shown = $derived(local ?? value);
  const text = $derived(shown === undefined ? '—' : shown.toFixed(decimals(Math.abs(snap(shown, min, max, step) - shown) < 1e-9 ? step : step / 10)));
  const atDef = $derived(shown === undefined || Math.abs(shown - def) < step / 20);

  function set(v: number, commit: boolean, fine = false) {
    if (off) return;
    if (start === null) sent = start = shown; // what the user saw (the rig may not have echoed it yet)
    const n = snap(v, min, max, fine ? step / 10 : step);
    local = n;
    live(n);
    if (commit) {
      live.flush();
      const prev = start;
      start = null;
      if (prev !== undefined && n !== prev) onchange?.(prev);
    }
  }
  function key(e: KeyboardEvent) {
    const d = { ArrowRight: 1, ArrowUp: 1, ArrowLeft: -1, ArrowDown: -1 }[e.key];
    if (!d || !(e.shiftKey || e.altKey)) return;
    e.preventDefault();
    const fine = !e.shiftKey && !onair;
    set((shown ?? def) + d * step * (e.shiftKey ? 10 : fine ? 0.1 : 1), true, fine);
  }
  function typed(el: HTMLInputElement) {
    const n = Number(el.value.replace(',', '.'));
    if (el.value.trim() === '' || !Number.isFinite(n)) el.value = text;
    else set(n, true, true);
  }
</script>

<div class="range {shown === undefined ? 'unknown' : ''}" title={locked ? t('stage.locked_live') : undefined}>
  <!-- svelte-ignore a11y_no_noninteractive_element_interactions -->
  <label for="{uid}-r" ondblclick={() => set(def, true, true)}>{label}{#if unit}<span class="unit">{` (${unit})`}</span>{/if}</label>
  <input
    id="{uid}-r"
    type="range"
    {min}
    {max}
    {step}
    value={shown ?? def}
    disabled={off}
    aria-valuetext={shown === undefined ? t('stage.fx_value_unknown') : `${text} ${unit ?? ''}`}
    style="--p:{(((shown ?? def) - min) / (max - min)) * 100}%"
    oninput={(e) => set(Number(e.currentTarget.value), false)}
    onchange={(e) => set(Number(e.currentTarget.value), true)}
    onkeydown={key}
  />
  <input class="num" inputmode="decimal" value={text} disabled={off} aria-label={t('stage.value_field', { label })} onchange={(e) => typed(e.currentTarget)} />
  <button type="button" class="rst" title={t('ui.reset_default')} aria-label="{t('ui.reset_default')}: {label}" disabled={off} hidden={atDef} onclick={() => set(def, true, true)}>↺</button>
  {#if ends}<span class="ends" aria-hidden="true"><span>{ends[0]}</span><span>{ends[1]}</span></span>{/if}
</div>

<style>
  @layer screens {
    .range {
      display: grid;
      grid-template-columns: minmax(0, 1fr) 6ch 24px;
      align-items: center;
      gap: 0 var(--sp-3);
      min-width: 0;
    }
    .range label { display: block; grid-column: 1 / -1; color: var(--c-text-muted); font-size: var(--fs-12); user-select: none; }
    input[type='range'] {
      -webkit-appearance: none;
      appearance: none;
      width: 100%;
      height: var(--hit-min);
      margin: 0;
      background: transparent;
    }
    input[type='range']::-webkit-slider-runnable-track {
      height: 4px;
      border-radius: var(--r-pill);
      background: linear-gradient(to right, var(--c-primary) var(--p), var(--c-border-strong) var(--p));
    }
    input[type='range']::-webkit-slider-thumb {
      -webkit-appearance: none;
      width: 16px;
      height: 16px;
      margin-top: -6px;
      border-radius: 50%;
      border: 2px solid var(--c-bg);
      background: var(--c-primary);
      box-shadow: 0 0 0 1px var(--c-primary);
    }
    input[type='range']:focus-visible { box-shadow: none; }
    input[type='range']:focus-visible::-webkit-slider-thumb { box-shadow: var(--focus-ring); }
    .unknown input[type='range']::-webkit-slider-runnable-track { background: var(--c-border-strong); }
    .unknown input[type='range']::-webkit-slider-thumb { background: var(--c-bg); border-color: var(--c-text-muted); box-shadow: none; }
    .num {
      width: 100%;
      height: 28px;
      padding: 0 var(--sp-3);
      border: 1px solid var(--c-border-strong);
      border-radius: var(--r-sm);
      background: var(--c-surface-2);
      color: var(--c-text);
      text-align: end;
      font: inherit;
      font-variant-numeric: tabular-nums;
    }
    .rst { width: 24px; height: 28px; padding: 0; border: 0; border-radius: var(--r-sm); background: none; color: var(--c-text-muted); }
    .rst:hover:not(:disabled) { background: var(--c-surface-3); color: var(--c-text); }
    .unit { color: var(--c-text-subtle); }
    .ends { display: flex; grid-column: 1 / 2; justify-content: space-between; color: var(--c-text-subtle); font-size: var(--fs-11); margin-top: -6px; }
  }
</style>
