<!--
  Copyright (C) 2026 Parallaxa Labs
  SPDX-License-Identifier: GPL-2.0-or-later
-->
<!--
  Saved views pad: a 3 x 3 grid of slots 1-9 = view_list / view_apply. The view on air
  (`view` of stage.state) gets the primary ring and an ON AIR label; a running transition draws
  a progress underline. An empty slot opens the save form; right-click, long-press (500 ms),
  the context-menu key or Shift+F10 on a saved slot opens its options (save current camera here,
  rename or change transition with view_save keep_pose, hold-to-delete; a plain group of buttons, so no aria-haspopup). Opening the menu or the form moves the focus into it; closing it (Esc,
  Cancel, Save) returns the focus to the pad. Pads always work, also while the on-air lock is on.
-->
<script lang="ts">
  import { tick } from 'svelte';
  import { SET_DURATION_MAX, TRANSITIONS, viewNameValid, type Transition } from '../lib/protocol/vendor.ts';
  import { holdable } from './hold.ts';
  import { SLOTS, type StageModel } from './model.svelte.ts';
  import { t } from './t.ts';

  let { m }: { m: StageModel } = $props();
  const uid = $props.id();

  /** Slot of the open save form (0 = none) and its fields. */
  let slot = $state.raw(0);
  let name = $state.raw('');
  let tr = $state.raw<Transition>('smooth');
  let ms = $state.raw(600);
  /** The form edits the stored view (view_save keep_pose) instead of saving the camera. */
  let keep = $state.raw(false);
  /** Slot whose menu is open (0 = none). */
  let menu = $state.raw(0);
  let longTimer: ReturnType<typeof setTimeout> | undefined;
  let longFired = false;
  const nameOk = $derived(viewNameValid(name));
  const del = holdable(() => void m.deleteView(menu).then(close));

  /** Focuses `${uid}-<id>` once the DOM shows it: 'm' menu, 'n' name field, a slot number = its pad. */
  const focus = (id: string | number) => void tick().then(() => document.getElementById(`${uid}-${id}`)?.focus());
  function close() {
    const n = menu || slot;
    slot = menu = 0;
    if (n) focus(n);
  }

  /** `detail` 0 = a keyboard click, which a long press never swallows. */
  function press(n: number, detail: number) {
    if (longFired) {
      longFired = false;
      if (detail) return; // the click that ends the long press
    }
    menu = 0;
    if (m.views[n - 1]) m.applyView(n);
    else edit(n);
  }
  function edit(n: number, keepPose = false) {
    const v = m.views[n - 1];
    keep = keepPose && !!v;
    name = v?.name ?? '';
    tr = v?.transition ?? 'smooth';
    ms = v?.duration_ms ?? 600;
    slot = n;
    menu = 0;
    focus('n');
  }
  function openMenu(n: number, e: Event | null) {
    if (!m.views[n - 1]) return;
    e?.preventDefault();
    menu = n;
    focus('m');
  }
  function down(n: number) {
    longFired = false;
    clearTimeout(longTimer);
    // Long-press only means something on a saved slot (its menu); an empty slot keeps its click.
    if (m.views[n - 1])
      longTimer = setTimeout(() => {
        longFired = true;
        openMenu(n, null);
      }, 500);
  }
  async function save(e: SubmitEvent) {
    e.preventDefault();
    if (!nameOk) return;
    const d = Math.round(Math.min(SET_DURATION_MAX, Math.max(0, ms || 0)));
    if (await m.saveView(slot, name, tr, tr === 'cut' ? 0 : d, keep)) close();
  }
  const esc = (e: KeyboardEvent) => e.key === 'Escape' && close();
</script>

<div class="grid" role="group" aria-label={t('stage.views')}>
  {#each SLOTS as n (n)}
    {@const v = m.views[n - 1]}
    {@const on = !!v && m.st?.view === n}
    <button
      id="{uid}-{n}"
      type="button"
      class="pad {v ? '' : 'empty'} {on ? 'on' : ''} {on && m.st?.moving ? 'moving' : ''}"
      aria-pressed={v ? on : undefined}
      aria-keyshortcuts={v ? 'Shift+F10' : undefined}
      onclick={(e) => press(n, e.detail)}
      oncontextmenu={(e) => openMenu(n, e)}
      onpointerdown={() => down(n)}
      onpointerup={() => clearTimeout(longTimer)}
      onpointerleave={() => clearTimeout(longTimer)}
      onkeydown={(e) => (e.key === 'ContextMenu' || (e.key === 'F10' && e.shiftKey)) && openMenu(n, e)}
    >
      <kbd class="n">{n}</kbd>
      <span class="name">{v ? v.name || t('stage.view_default_name', { slot: n }) : '+ ' + t('stage.save_view')}</span>
      {#if on}<span class="air">{t('stage.on_air')}</span>{/if}
    </button>
  {/each}
</div>

{#if menu}
  <!-- svelte-ignore a11y_no_noninteractive_element_interactions -->
  <div class="box" role="group" aria-label={t('stage.view_default_name', { slot: menu })} onkeydown={esc}>
    <button id="{uid}-m" type="button" class="b" onclick={() => edit(menu)}>{t('stage.save_view_here')}</button>
    <button type="button" class="b" onclick={() => edit(menu, true)}>{t('stage.edit_view')}</button>
    <button
      type="button"
      class="b d"
      aria-describedby="{uid}-h"
      onpointerdown={del.start}
      onkeydown={del.start}
      onpointerup={del.stop}
      onpointerleave={del.stop}
      onkeyup={del.stop}
      onblur={del.stop}>{t('stage.delete')}</button
    >
    <span id="{uid}-h" class="note">{t('stage.hold_confirm')}</span>
    <button type="button" class="b g" onclick={close}>{t('stage.cancel')}</button>
  </div>
{/if}

{#if slot}
  <!-- svelte-ignore a11y_no_noninteractive_element_interactions -->
  <form class="box" onsubmit={save} onkeydown={esc}>
    <strong>{t('stage.view_default_name', { slot })}</strong>
    <label>
      <span>{t('stage.view_name')}</span>
      <input id="{uid}-n" value={name} oninput={(e) => (name = e.currentTarget.value)} placeholder={t('stage.view_default_name', { slot })} aria-invalid={!nameOk} />
    </label>
    {#if !nameOk}<span class="note err" role="alert">{t('stage.view_name_long')}</span>{/if}
    <div class="two">
      <label>
        <span>{t('stage.transition')}</span>
        <!-- {@attach}, not value=: see StageControl.svelte. -->
        <select {@attach (el) => void (el.value = tr)} onchange={(e) => (tr = e.currentTarget.value as Transition)}>
          {#each TRANSITIONS as x (x)}<option value={x}>{t(`stage.transition.${x}`)}</option>{/each}
        </select>
      </label>
      <label>
        <span>{t('stage.view_time')}</span>
        <input type="number" min="0" max={SET_DURATION_MAX} step="50" value={ms} oninput={(e) => (ms = Number(e.currentTarget.value))} disabled={tr === 'cut'} />
      </label>
    </div>
    <span class="note">{t(keep ? 'stage.view_keeps_pose' : 'stage.view_saves_pose')}</span>
    <div class="row">
      <button type="submit" class="b p" disabled={!nameOk}>{t('stage.save')}</button>
      <button type="button" class="b g" onclick={close}>{t('stage.cancel')}</button>
    </div>
  </form>
{/if}

<style>
  @layer screens {
    .grid { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: var(--sp-4); }
    .pad {
      position: relative;
      display: flex;
      flex-direction: column;
      align-items: flex-start;
      justify-content: space-between;
      gap: var(--sp-2);
      aspect-ratio: 1;
      min-height: var(--hit-touch);
      padding: var(--sp-4);
      border: 1px solid var(--c-border-strong);
      border-radius: var(--r-lg);
      background: var(--c-surface-2);
      color: var(--c-text);
      text-align: start;
      overflow: hidden;
      transition: transform var(--dur-1) var(--ease-spring), box-shadow var(--dur-2) var(--ease-out);
      -webkit-touch-callout: none;
      user-select: none;
    }
    .pad:hover { background: var(--c-surface-3); }
    .pad:active { transform: scale(0.96); }
    .empty { border-style: dashed; background: none; color: var(--c-text-muted); }
    .on { border-color: var(--c-primary); box-shadow: var(--glow-primary); }
    .on:focus-visible { box-shadow: var(--focus-ring), var(--glow-primary); }
    .n { font: var(--fw-bold) var(--fs-18) / 1 var(--font-display); color: inherit; }
    .name {
      font: var(--fw-medium) var(--fs-13) / var(--lh-tight) var(--font-display);
      display: -webkit-box;
      -webkit-line-clamp: 2;
      line-clamp: 2;
      -webkit-box-orient: vertical;
      overflow: hidden;
      overflow-wrap: anywhere;
    }
    .empty .name { font-size: var(--fs-12); }
    .air { position: absolute; top: var(--sp-3); right: var(--sp-3); font-size: 10px; font-weight: var(--fw-bold); color: var(--c-primary); }
    .moving::after {
      content: '';
      position: absolute;
      inset: auto 0 0;
      height: 3px;
      background: var(--c-primary);
      transform-origin: left;
      animation: stage-fill 600ms linear infinite;
    }
    .box {
      display: flex;
      flex-direction: column;
      gap: var(--sp-4);
      padding: var(--sp-5);
      border: 1px solid var(--c-border-strong);
      border-radius: var(--r-md);
      background: var(--c-surface-2);
    }
    .two { display: grid; grid-template-columns: 1fr 1fr; gap: var(--sp-4); }
  }
</style>
